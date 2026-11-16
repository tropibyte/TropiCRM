# TropiCRM

TropiCRM is a multi-tenant CRM server written in portable C++20. It follows
the shape of the Salesforce platform: standard sales and service objects,
metadata-driven custom objects and fields, a SOQL-style query language,
formula fields, validation and workflow rules, approvals, role-based
sharing, reports and dashboards, bulk CSV ingest and outbound webhooks — all
behind a JSON REST API.

The server has no third-party dependencies. HTTP, JSON, SHA-256/HMAC/PBKDF2,
the storage engine, the query engine and the formula engine are all part of
this repository.

```
┌──────────────┐   HTTP/1.1 + JSON   ┌───────────────────────────────────────────┐
│ REST clients │ ──────────────────▶ │ tropicrm-server                           │
└──────────────┘                     │  router → auth → API handlers             │
                                     │  record service · query · formula · rules │
                                     │  per-org schema, sharing, WAL storage     │
                                     └───────────────────────────────────────────┘
```

## Features

| Area | What you get |
| --- | --- |
| Tenancy | Self-service signup creates an isolated org with its own schema, users, data and settings. Platform operators get a separate admin API. |
| Objects | Account, Contact, Lead, Opportunity, Opportunity Product, Product, Case, Task, Event, Campaign, Note, Attachment and User, plus custom objects (`Thing__c`). |
| Fields | Text, long text, email, phone, URL, number, currency, percent, checkbox, date, date/time, picklist, multi-select picklist, lookup, master-detail, auto-number, formula and roll-up summary. |
| Records | Create, read, update, delete, upsert by external ID, undelete from the recycle bin, field history, ETags and conditional requests. |
| Query | TQL, a SOQL-like language with parent and child relationships, aggregates, `GROUP BY`/`HAVING`, relative date literals, `INCLUDES`/`EXCLUDES` and server-side cursors. |
| Search | Full-text search across searchable fields with per-object filters. |
| Security | Profiles with object CRUD and field-level security, a role hierarchy, org-wide defaults, criteria and owner sharing rules, manual shares, API keys, account lockout. |
| Automation | Validation rules, workflow rules (field updates, tasks, webhooks, email alerts), lead and case assignment rules, duplicate checks. |
| Processes | Lead conversion, multi-step approvals with record locking, record merge, mass ownership transfer. |
| Analytics | Tabular and summary reports (JSON or CSV), dashboards built from reports, opportunity forecasts by role. |
| Integration | Bulk CSV ingest jobs (insert, update, upsert, delete), signed webhooks, composite requests, Web-to-Lead and Web-to-Case, email templates with merge fields. |
| Operations | Write-ahead log with CRC-checked lines, periodic snapshots, rate limiting, request metrics, setup audit trail, health checks and an admin CLI. |

## Building

You need CMake 3.20 or newer and a C++20 compiler (GCC 11+, Clang 14+ or
MSVC 2022). Linux, macOS and Windows are supported; Windows builds link
against Winsock.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -j
```

The build produces:

| Target | Description |
| --- | --- |
| `tropicrm-server` | The REST server. |
| `tropicrm-admin` | Offline maintenance CLI that works directly on a data directory. |
| `tropicrm_tests` | Unit and API tests. Each suite is registered with CTest separately. |

Useful CMake options:

| Option | Default | Effect |
| --- | --- | --- |
| `TROPICRM_SANITIZE` | `OFF` | Builds with AddressSanitizer and UndefinedBehaviorSanitizer. |
| `TROPICRM_TSAN` | `OFF` | Builds with ThreadSanitizer. |
| `TROPICRM_WARNINGS_AS_ERRORS` | `OFF` | Treats compiler warnings as errors. |
| `TROPICRM_BUILD_TESTS` | `ON` at top level | Builds the test executable. |
| `TROPICRM_BUILD_TOOLS` | `ON` | Builds `tropicrm-admin`. |

Run a single test suite with `./build/tests/tropicrm_tests --suite query_exec`,
or list suites with `--list`.

## Running

```sh
./build/tropicrm-server --data ./data --port 8080
```

Settings are read in this order, later sources winning:

1. Built-in defaults.
2. An INI file passed with `--config` (see `tropicrm.ini.example`).
3. Environment variables named `TROPICRM_<SECTION>_<KEY>`, for example
   `TROPICRM_SERVER_PORT=9000`.
4. Command-line flags (`--data`, `--port`, `--log-level`).

The server writes one directory per org under the data directory. Stop it
with Ctrl+C or SIGTERM; it takes a final snapshot of every org before it
exits.

## Quick start

The examples use `curl` and `jq`. Pick your own login and password; logins
are email-style usernames and must be unique across the server.

```sh
export TCRM=http://localhost:8080
export TCRM_USER='<your login>'
export TCRM_PASS='<8+ characters with a letter and a digit>'

# 1. Create an org. The caller becomes its System Administrator.
curl -s $TCRM/services/signup -H 'Content-Type: application/json' -d '{
  "company": "Acme Corp",
  "username": "'"$TCRM_USER"'",
  "firstName": "Ada",
  "lastName": "Admin",
  "password": "'"$TCRM_PASS"'"
}'

# 2. Log in with the OAuth password grant.
export TOKEN=$(curl -s $TCRM/services/oauth2/token \
  --data-urlencode grant_type=password \
  --data-urlencode username="$TCRM_USER" \
  --data-urlencode password="$TCRM_PASS" | jq -r .access_token)

# 3. Create an account and a contact under it.
ACCT=$(curl -s $TCRM/services/data/v1/sobjects/Account \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"Name": "Globex", "Industry": "Manufacturing"}' | jq -r .id)

curl -s $TCRM/services/data/v1/sobjects/Contact \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"FirstName": "Hank", "LastName": "Scorpio", "AccountId": "'"$ACCT"'"}'

# 4. Query with a parent relationship.
curl -s -G $TCRM/services/data/v1/query -H "Authorization: Bearer $TOKEN" \
  --data-urlencode "q=SELECT Name, Account.Name FROM Contact ORDER BY Name"
```

Next steps:

* [docs/API.md](docs/API.md) — every endpoint, request and response.
* [docs/QUERY.md](docs/QUERY.md) — the TQL query language.
* [docs/FORMULAS.md](docs/FORMULAS.md) — formula syntax and functions.
* [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the server is put together.

## Admin CLI

`tropicrm-admin` operates on a data directory while the server is stopped.

```
tropicrm-admin --data DIR <command> [arguments]

  orgs                                   list orgs
  create-org NAME USERNAME PASSWORD      create an org and its first admin
  users ORG_ID                           list an org's users
  query USERNAME "SELECT ..."            run a query as a user, print JSON
  export ORG_ID OBJECT [FILE]            write every record of an object as CSV
  import USERNAME OBJECT FILE            insert records from a CSV file
  snapshot                               compact every org's log into a snapshot
  check ORG_ID                           verify references, indexes and roll-ups
  rebuild-indexes ORG_ID                 drop and rebuild every secondary index
  reset-password USERNAME NEW_PASSWORD   set a user's password
  export-org ORG_ID FILE                 write the org's metadata and records as JSON
  hash-key KEY                           print the SHA-256 for platform.admin_key_sha256
```

## Repository layout

```
src/
  util/       strings, time zones, crypto, URLs, CSV, config, logging, thread pool
  json/       JSON value type, parser and serializer
  http/       sockets, HTTP parser, router, server and a small client
  model/      field values, records and API errors
  meta/       field and object definitions, the schema, standard objects
  storage/    tables, indexes, write-ahead log, snapshots
  auth/       users, password hashing, tokens
  security/   profiles and the sharing model
  formula/    formula parser and evaluator
  query/      TQL parser and executor
  engine/     orgs, access checks, the record pipeline, automation, approvals
  services/   search, webhooks, files, reports, dashboards, bulk, templates, audit
  api/        REST route handlers and authentication
  app/        application wiring and the server entry point
tools/        tropicrm-admin
tests/        test framework, in-process API harness and test suites
docs/         reference documentation
```

## License

MIT. See [LICENSE](LICENSE).

Copyright (c) 2026 Tropibyte, Inc.
