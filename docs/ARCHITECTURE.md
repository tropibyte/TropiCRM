# Architecture

This document describes how a request moves through TropiCRM and how the
main subsystems fit together. The source tree mirrors these layers; see the
layout in the [README](../README.md#repository-layout).

```
            ┌──────────────────────────── http ────────────────────────────┐
socket ───▶ │ Server (accept thread + worker pool) → Parser → Request      │
            └──────────────────────────────┬───────────────────────────────┘
                                           ▼
            ┌──────────────────────── app::Application ─────────────────────┐
            │ CORS preflight · public rate limit · metrics · error mapping │
            │ Router: method + path pattern → handler                      │
            └──────────────────────────────┬───────────────────────────────┘
                                           ▼
            ┌───────────────────────────── api ────────────────────────────┐
            │ authed(): token / API key → Principal · daily API limit      │
            │ route handlers: parse JSON, call services, shape responses   │
            └──────────────────────────────┬───────────────────────────────┘
                                           ▼
   ┌──────────────── engine ────────────────┐   ┌──────────── services ────────────┐
   │ OrgRegistry → Org (schema, users,      │   │ search · webhooks · files ·      │
   │   sharing, automation, settings, db)   │◀─▶│ reports · dashboards · bulk ·    │
   │ AccessChecker (CRUD, FLS, sharing)     │   │ outbox · templates · audit       │
   │ RecordService (save pipeline)          │   └──────────────────────────────────┘
   │ approvals · lead convert · merge       │
   └───────┬──────────────┬─────────────────┘
           ▼              ▼
   ┌── query ──┐   ┌── formula ──┐   ┌──────────── storage ────────────┐
   │ TQL parse │   │ parse, eval │   │ Database: tables, indexes, WAL, │
   │ execute   │   │ (cached)    │   │ snapshots, metadata entries     │
   └───────────┘   └─────────────┘   └─────────────────────────────────┘
```

---

## HTTP layer (`src/http`)

`http::Server` owns a listening socket, one accept thread and a
`ThreadPool` of `server.workers` threads. Each accepted connection is
handed to a worker, which loops: read bytes, feed the incremental
`http::Parser`, dispatch a complete `Request`, write the `Response`, and
repeat for keep-alive connections up to `server.keepalive_requests`.

The parser handles request lines, headers (limited to
`server.max_header_bytes`), `Content-Length` and chunked bodies (limited to
`server.max_body_bytes`). `socket.hpp` hides the POSIX/Winsock differences.

`http::Router` matches the method and path against patterns registered at
startup. `{name}` captures one segment and `{*name}` captures the rest. A
path that matches with the wrong method yields `405`.

`http::client` is a minimal blocking HTTP/1.1 client used for webhook
delivery.

## Application (`src/app`)

`Application` builds everything from the `Config`, registers the routes and
implements `handle(Request) → Response`, which is what the server calls and
what the tests call directly (no sockets). `handle()`:

1. Answers CORS preflight requests and adds CORS headers.
2. Applies the per-address rate limit to unauthenticated endpoints
   (`/services/oauth2/`, `/services/auth/`, `/services/web2*`,
   `/services/signup`).
3. Routes the request and converts any `ApiError` into the JSON error
   format; unexpected exceptions become `500`.
4. Records metrics and an access-log line.

A maintenance thread runs every minute: it snapshots orgs whose logs have
grown past `data.snapshot_every` entries, purges expired recycle-bin
records, and prunes rate-limiter buckets.

## API layer (`src/api`)

Each `*_routes.cpp` registers a group of endpoints against an
`ApiContext`, a struct of references to the shared services. Handlers that
need a user are wrapped in `authed()`, which:

* resolves the `Authorization` header to an `engine::Principal` (the user,
  their org, profile and role) through the `Authenticator`;
* rejects inactive users, suspended orgs and profiles without API access;
* counts the call against the org's daily allowance and sets
  `Sforce-Limit-Info`.

Handlers stay thin: validate input, call the engine or a service, and turn
the result into JSON. `helpers.cpp` holds the shared pieces (error
responses, describe output, JSON body parsing).

## Tenancy and orgs (`src/engine/org.*`)

Every tenant is an `Org`:

| Member | Contents |
| --- | --- |
| `schema` | Standard objects plus the org's custom objects and fields. |
| `users` | Users, password hashes, API keys, lockout state. |
| `profiles`, `roles` | Permission sets and the role hierarchy. |
| `sharing` | Org-wide defaults, sharing rules and manual shares. |
| `automation` | Validation, workflow and assignment rules. |
| `settings` | Time zone, currency, limits, business hours, feature flags. |
| `db` | The org's `storage::Database`. |

`OrgRegistry` loads every org directory at startup, creates new orgs and
keeps a username → org index for login.

### Concurrency

Each org has a `std::shared_mutex`. Reads (queries, describes, record GETs)
take it shared; anything that changes the org takes it exclusively. Orgs
never lock each other, so tenants don't contend. Services with their own
state (search indexes, cursors, webhooks, bulk jobs) have their own
mutexes, and must not be called while holding them if they might call back
into an org.

## Metadata (`src/meta`)

`FieldDef` and `ObjectDef` describe fields and objects. `standard_objects.cpp`
defines the built-in objects. `Schema` holds an org's objects, validates
custom definitions, and rebuilds child relationships whenever a lookup is
added or removed.

`codec.cpp` converts between API JSON and stored `FieldValue`s per field
type, enforcing lengths, precision, picklist values and formats.

## Security model (`src/security`, `src/engine/access.*`)

An `AccessChecker` is built for a `Principal` and answers:

* **Object permissions** — may this profile read, create, edit or delete
  this object at all?
* **Field-level security** — may it read or edit this field?
* **Record access** — what access (`None`, `Read`, `Edit`, `All`) does this
  user have to this record?

Record access is the highest of: administrator permissions (*View All*,
*Modify All*, *View All Data*, *Modify All Data*), the org-wide default,
ownership (by the user or anyone below them in the role hierarchy), sharing
rules for the user's role and the roles above it, manual shares, and for
`ControlledByParent` objects the access to the parent record.

The role hierarchy is cached as a parent map; "roles below" is computed by
walking it.

## The save pipeline (`src/engine/record_service.*`)

All writes — REST, composite, bulk, lead conversion, merge, workflow field
updates — go through `RecordService`, under the org's exclusive lock:

1. **Resolve** the object and check object permissions.
2. **Apply fields** from the request: field-level security, read-only
   checks, type conversion (`codec`), reference checks (the target exists,
   has the right type, and is visible).
3. **Defaults and auto-numbers** on create.
4. **System fields**: created/modified dates and users, `SystemModstamp`.
5. **Formulas** are recomputed.
6. **Required and unique** fields are checked.
7. **Validation rules** run.
8. **Duplicate check** (Email or Name) when the org enables it.
9. **Store** the record (WAL append and in-memory table update).
10. **Field history** for tracked fields.
11. **Roll-ups** on master records are recalculated.
12. **Hooks** notify the search index and other listeners.
13. **Workflow rules** run; their field updates re-enter the pipeline.

Deletes check access and delete rules, cascade to master-detail children,
clear or block lookups, and move records to the recycle bin
(`IsDeleted = true`). Undelete reverses this for the record and the children
deleted with it.

## Formulas (`src/formula`)

`parser.cpp` turns formula text into an AST with precedence climbing.
`evaluator.cpp` walks the AST against a `Context`, which supplies field
values (including lookups through relationships), the prior record, the
running user and the org's time zone. `RecordFormulaContext` in the engine
implements `Context` for records. Parsed formulas are cached by source text.

## Queries (`src/query`)

`parser.cpp` turns TQL into a `Query` AST. `Executor`:

1. resolves every field path against the schema, checking FLS and
   relationship depth;
2. picks a unique or secondary index for an equality filter when it can,
   otherwise scans the table;
3. filters rows with the `WHERE` clause and the caller's record access;
4. sorts, groups and aggregates, applies `HAVING`, `OFFSET` and `LIMIT`;
5. builds result rows, nesting parent fields and running child subqueries.

Results larger than one batch are parked in a `CursorStore` and paged out
through `nextRecordsUrl`.

## Storage (`src/storage`)

Each org has a `Database` under `<data.dir>/orgs/<orgId>/`:

```
orgs/00D5g00000AbCdE/
  snapshot.jsonl   # full image of every table and metadata entry
  wal.log          # changes since the snapshot
  files/           # attachment bodies
```

The write-ahead log is append-only. Each line is

```
<crc32 hex> <length> <json>\n
```

and the JSON is either a record (`put`), a delete marker or a metadata
entry (`schema`, `users`, `sharing`, `settings`, `automation`, …). On
startup the database loads the snapshot and replays the log, verifying
every line's checksum and length. A snapshot writes the whole state to
`snapshot.jsonl` and truncates the log.

`Table` keeps records in memory, keyed by ID, with optional unique and
secondary indexes on individual fields. The whole working set lives in
memory; the log and snapshots make it durable.

With `data.dir` set to an empty string the database is in-memory only, which
is what the tests use.

## Services (`src/services`)

| Service | Notes |
| --- | --- |
| `SearchService` | An inverted index per org, updated by the save hook, with a TF-IDF-style score. Rebuilt at startup. |
| `WebhookService` | A queue with worker threads, HMAC signatures, retries with exponential backoff and a log of recent deliveries. |
| `FileStore` | Attachment bodies on disk under the org's `files/` directory. |
| `ReportService` | Turns report definitions into TQL, runs them as the caller and formats JSON or CSV. |
| `Dashboards` | Runs a dashboard's component reports, optionally as a fixed running user. |
| `BulkService` | Ingest jobs: CSV parsing, background processing on a thread pool, success and failure results. |
| `Outbox` | Outgoing email (password resets, alerts, template sends), kept for inspection; no SMTP. |
| `Templates` | Email templates with `{!Field}` merge fields. |
| `AuditTrail` | Setup changes, kept per org in a bounded ring. |
| `RecentItems` | Each user's recently viewed records. |

## Authentication (`src/auth`, `src/api/authenticator.*`)

Passwords are hashed with PBKDF2-HMAC-SHA256 and a random salt
(`auth.pbkdf2_iterations`). Access tokens are signed with HMAC-SHA256 using
a key stored in `<data.dir>/signing.key`, and carry the org, user and
expiry. Refresh tokens and API keys are random and stored as SHA-256
hashes. Failed logins are counted per user and lead to a temporary lockout.

## Testing (`tests`)

`testing.hpp` is a small test framework: `TEST(suite, name)`, `CHECK`,
`CHECK_EQ`, `CHECK_NEAR`, `CHECK_THROWS_AS`, `CHECK_API_ERROR`, and `REQUIRE`
variants that stop the test. Each suite is a separate CTest entry.

`harness.hpp` provides `TestApp`, which builds an `Application` with
in-memory storage and drives it through `handle()`, and `Fixture`, which
signs up an org and logs in its administrator. Test logins are built at run
time so the sources contain no real addresses.

## Portability

The code targets C++20 with no compiler extensions. Platform differences are
confined to `http/socket.*` (POSIX sockets vs Winsock). Calendar math uses
Howard Hinnant's civil-date algorithms rather than the C library, so it
doesn't depend on the server's time zone. Builds are warning-clean with GCC
and Clang at `-Wall -Wextra -Wpedantic` and with MSVC at `/W4`.
