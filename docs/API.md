# TropiCRM REST API

All endpoints accept and return JSON (`Content-Type: application/json`) unless
noted. Data endpoints live under `/services/data/v1`; the examples below
abbreviate that prefix to `…/`.

* [Conventions](#conventions)
* [Authentication](#authentication)
* [Discovery and limits](#discovery-and-limits)
* [Records](#records)
* [Query and search](#query-and-search)
* [Composite requests](#composite-requests)
* [Users and API keys](#users-and-api-keys)
* [Setup (Tooling API)](#setup-tooling-api)
* [Sharing](#sharing)
* [Actions](#actions)
* [Approvals](#approvals)
* [Email templates](#email-templates)
* [Reports, dashboards and forecasts](#reports-dashboards-and-forecasts)
* [Bulk ingest](#bulk-ingest)
* [Webhooks](#webhooks)
* [Files](#files)
* [Web-to-Lead and Web-to-Case](#web-to-lead-and-web-to-case)
* [Audit, recent items and health](#audit-recent-items-and-health)
* [Platform administration](#platform-administration)

---

## Conventions

### IDs

Record IDs follow the Salesforce format: a three-character key prefix that
names the object (`001` Account, `003` Contact, `00Q` Lead, `006` Opportunity,
`500` Case, …), followed by twelve base-62 characters. Every API that takes an
ID accepts the case-sensitive 15-character form and the case-insensitive
18-character form; responses always use 18 characters.

### Field names

Field names are case-insensitive on input and returned with their canonical
spelling. Custom objects and custom fields end in `__c`. Relationship fields
end in `Id` for standard lookups and `__c` for custom ones; the relationship
itself is named without the `Id` suffix (`Account`) or with `__r`
(`Project__r`).

### Values

| Field type | JSON representation |
| --- | --- |
| text, email, phone, URL, picklist | string |
| multi-select picklist | string of values separated by `;`, or an array of strings on input |
| number, currency, percent | number |
| checkbox | `true` / `false` |
| date | `"YYYY-MM-DD"` |
| date/time | `"YYYY-MM-DDThh:mm:ss.sssZ"` (UTC); offsets are accepted on input |
| lookup / master-detail | record ID string |

`null` clears a field. Read-only fields (system fields, formulas, roll-ups,
auto-numbers) are rejected on create and update.

### Errors

Errors use HTTP status codes and a JSON array body:

```json
[
  {
    "errorCode": "REQUIRED_FIELD_MISSING",
    "message": "Required fields are missing: [LastName]",
    "fields": ["LastName"]
  }
]
```

| Status | Typical codes |
| --- | --- |
| 400 | `INVALID_FIELD`, `REQUIRED_FIELD_MISSING`, `FIELD_CUSTOM_VALIDATION_EXCEPTION`, `MALFORMED_QUERY`, `INVALID_QUERY_LOCATOR`, `DUPLICATE_VALUE`, `ENTITY_IS_LOCKED`, OAuth errors (`invalid_grant`, …) |
| 401 | `INVALID_SESSION_ID` |
| 403 | `INSUFFICIENT_ACCESS`, `INSUFFICIENT_ACCESS_OR_READONLY`, `API_DISABLED_FOR_ORG`, `WEB_FORMS_DISABLED` |
| 404 | `NOT_FOUND`, `ENTITY_IS_DELETED` |
| 405 | `METHOD_NOT_ALLOWED` |
| 409 | conflicts such as a duplicate username |
| 412 | `PRECONDITION_FAILED` |
| 413 | request body larger than `server.max_body_bytes` |
| 429 | `REQUEST_LIMIT_EXCEEDED` — the org's daily allowance, or the per-address limit on public endpoints (with `Retry-After`) |
| 500 | `UNKNOWN_EXCEPTION` |

### Usage header

Every authenticated response carries
`Sforce-Limit-Info: api-usage=<used>/<limit>` for the org's daily API
allowance.

### CORS

Cross-origin requests are allowed from the origins listed in
`cors.allowed_origins` (comma-separated). Preflight requests are answered
without authentication.

---

## Authentication

Authenticated endpoints take one of:

* `Authorization: Bearer <access_token>` — from the token endpoint.
* `Authorization: ApiKey <key>` — a long-lived key created by a user (see
  [API keys](#users-and-api-keys)).

### `POST /services/signup`

Creates an org and its first user, who gets the System Administrator profile.

```json
{
  "company": "Acme Corp",
  "username": "<login>",
  "firstName": "Ada",
  "lastName": "Admin",
  "password": "<password>"
}
```

`email` is optional and defaults to the username.

Response `201`:

```json
{"orgId": "00D…", "username": "<login>", "loginUrl": "/services/oauth2/token"}
```

Passwords must be at least eight characters and contain a letter and a digit.
Usernames are unique across the whole server.

### `POST /services/oauth2/token`

Form-encoded (`application/x-www-form-urlencoded`).

| Grant | Parameters |
| --- | --- |
| `password` | `username`, `password` |
| `refresh_token` | `refresh_token` |

Response `200`:

```json
{
  "access_token": "…",
  "refresh_token": "…",
  "token_type": "Bearer",
  "instance_url": "",
  "id": "/services/oauth2/userinfo",
  "org_id": "00D…",
  "user_id": "005…",
  "issued_at": "1763300000000",
  "expires_in": 7200
}
```

After `auth.max_failed_logins` bad passwords in a row the user is locked out
for `auth.lockout_minutes`. Inactive users and users of suspended orgs cannot
log in.

### `POST /services/oauth2/revoke`

Form parameter `token`: revokes an access or refresh token. Always returns
`200`.

### `GET /services/oauth2/userinfo`

Returns the caller's user ID, org ID, name, profile, role, locale and time
zone.

### `POST /services/auth/password/forgot`

`{"username": "<login>"}`. Queues a reset code to the user's email address
through the outbox. Always returns `200` with the same message, whether or
not the user exists.

### `POST /services/auth/password/reset`

`{"username": "<login>", "code": "<code>", "newPassword": "<password>"}`.
Codes expire after `auth.password_reset_ttl_minutes` and can be used once.
Returns `200` with `{"success": true}`.

---

## Discovery and limits

### `GET /services/data`

Lists the available API versions. No authentication.

### `GET …/`

Lists the top-level resources for v1.

### `GET …/limits`

The org's daily API allowance and usage, storage use, and data limits.

### `GET …/sobjects`

Every object the caller can see, with its label, key prefix and CRUD
permissions.

### `GET …/sobjects/{object}/describe`

Full metadata for one object: fields (type, length, precision, picklist
values, reference targets, required/unique/externalId flags, and
`createable`/`updateable`/`calculated` from the caller's point of view), child
relationships and record URLs. Fields the caller's profile hides are omitted.

### `GET …/sobjects/{object}`

Object summary plus the caller's most recently viewed records of that type.

---

## Records

### Create — `POST …/sobjects/{object}`

```json
{"Name": "Globex", "Industry": "Manufacturing", "AnnualRevenue": 2500000}
```

Response `201` with a `Location` header:

```json
{"id": "001…", "success": true, "errors": []}
```

`OwnerId` defaults to the caller. Defaults, auto-numbers, formulas and
roll-ups are filled in by the server.

Optional headers:

| Header | Effect |
| --- | --- |
| `Sforce-Auto-Assign: true` | Runs active assignment rules on Leads and Cases. |
| `Sforce-Duplicate-Rule-Header: allowSave=true` | Saves even when the duplicate check finds a match (needs the org's `duplicateRules` setting). |

### Read — `GET …/sobjects/{object}/{id}`

Returns every readable field, or only those in `?fields=Name,Industry`. The
response carries an `ETag`; send it back in `If-None-Match` to get `304 Not
Modified` when nothing changed.

### Update — `PATCH …/sobjects/{object}/{id}`

Sends only the fields to change. Returns `204`. With `If-Match: "<etag>"` the
update fails with `412` if someone else changed the record first.

Records in an approval process are locked: only administrators and the
current approver can edit them.

### Delete — `DELETE …/sobjects/{object}/{id}`

Moves the record to the recycle bin and returns `204`. Child records follow
the relationship's delete rule: master-detail children are deleted with the
parent, lookups are cleared, and restricted lookups block the delete.

### Upsert — `PATCH …/sobjects/{object}/{externalIdField}/{value}`

Updates the record whose external ID field equals `value`, or creates one.
Returns `200` for an update and `201` for an insert. The field must be marked
`externalId` and is matched case-insensitively.

### History — `GET …/sobjects/{object}/{id}/history`

Field-level changes to the record, newest first:

```json
{
  "totalSize": 1,
  "done": true,
  "records": [
    {"Id": "017…", "ParentId": "006…", "Field": "StageName", "OldValue": "Prospecting",
     "NewValue": "Closed Won", "CreatedById": "005…", "CreatedDate": "2026-11-02T15:04:05.000Z"}
  ]
}
```

### Activities — `GET …/sobjects/{object}/{id}/activities`

Tasks and Events related to the record through `WhatId` or `WhoId`, newest
first.

### Recycle bin

| Request | Effect |
| --- | --- |
| `GET …/recyclebin` | Deleted records the caller can see. |
| `POST …/recyclebin/{id}/undelete` | Restores a record and the children deleted with it. |
| `DELETE …/recyclebin/{id}` | Purges the record permanently. |

Deleted records are purged automatically after the org's `recycleBinDays`.

---

## Query and search

### `GET …/query?q=<TQL>`

Runs a TQL query (see [QUERY.md](QUERY.md)) with the caller's sharing and
field-level security applied.

```json
{
  "totalSize": 3120,
  "done": false,
  "nextRecordsUrl": "/services/data/v1/query/01g…",
  "records": [
    {"attributes": {"type": "Contact", "url": "/services/data/v1/sobjects/Contact/003…"},
     "Id": "003…", "Name": "Hank Scorpio", "Account": {"Name": "Globex"}}
  ]
}
```

Results come back in batches of 2,000 records by default. Set
`Sforce-Query-Options: batchSize=500` to change this (200–2,000). Fetch the
next batch from `nextRecordsUrl`. Cursors expire after fifteen minutes of
inactivity.

### `GET …/queryAll?q=<TQL>`

Like `query`, but includes records in the recycle bin (`IsDeleted = true`).

### `GET …/query/{cursor}`

The next batch of an earlier query.

### `GET …/search?q=<text>&objects=Account,Contact&limit=25`

Full-text search across searchable fields. Every term must appear in a
record for it to match; the last term also matches as a prefix, so partial
input works for type-ahead. Results are ranked by a TF-IDF style score that
favours exact and rare terms in short records. `limit` is 1–200 (default 25).

```json
{
  "searchRecords": [
    {"attributes": {"type": "Account", "url": "…"}, "Id": "001…", "Name": "Globex", "score": 2.31}
  ]
}
```

---

## Composite requests

### `POST …/composite`

Runs up to 25 subrequests in order. Later subrequests can refer to the
results of earlier ones with `@{referenceId.field}`.

```json
{
  "allOrNone": true,
  "compositeRequest": [
    {"method": "POST", "url": "/services/data/v1/sobjects/Account",
     "referenceId": "acct", "body": {"Name": "Initech"}},
    {"method": "POST", "url": "/services/data/v1/sobjects/Contact",
     "referenceId": "boss", "body": {"LastName": "Lumbergh", "AccountId": "@{acct.id}"}}
  ]
}
```

Response:

```json
{
  "compositeResponse": [
    {"referenceId": "acct", "httpStatusCode": 201, "httpHeaders": {}, "body": {"id": "001…", "success": true, "errors": []}},
    {"referenceId": "boss", "httpStatusCode": 201, "httpHeaders": {}, "body": {"id": "003…", "success": true, "errors": []}}
  ]
}
```

With `allOrNone`, a failed subrequest stops processing and the records
created earlier in the request are rolled back.

---

## Users and API keys

Users are managed by anyone whose profile has *Manage Users*.

| Request | Effect |
| --- | --- |
| `GET …/users` | Lists users in the org. |
| `GET …/users/{id}` | One user. |
| `POST …/users` | Creates a user (`username`, `firstName`, `lastName`, `email`, `profileName` or `profileId`, `roleId`, `managerId`, `password`, `timezone`, `locale`, `title`, `department`). Without a password the user is sent a reset code. |
| `PATCH …/users/{id}` | Updates a user. Set `"active": false` to deactivate. |
| `POST …/users/{id}/password` | `{"newPassword": …}`. Users changing their own password also send `currentPassword`. |

Users can also be read through TQL: `SELECT Name, Title FROM User`.

API keys belong to the user who creates them and carry that user's access:

| Request | Effect |
| --- | --- |
| `GET …/users/me/apikeys` | Lists the caller's keys (ID, prefix and label only). |
| `POST …/users/me/apikeys` | `{"label": "ci"}`; returns the key once. |
| `DELETE …/users/me/apikeys/{keyId}` | Revokes a key. |

---

## Setup (Tooling API)

Setup endpoints need the *Customize Application* permission unless noted.

### Custom objects and fields

| Request | Effect |
| --- | --- |
| `GET …/tooling/objects` | Lists objects with their custom field counts. |
| `POST …/tooling/objects` | Creates a custom object: `{"name": "Project__c", "label": "Project", "pluralLabel": "Projects"}`. |
| `POST …/tooling/objects/{object}/fields` | Adds a custom field (see below). |
| `PATCH …/tooling/objects/{object}/fields/{field}` | Changes a field's label, help text, picklist values, length, required, unique or externalId flags, or formula. |
| `DELETE …/tooling/objects/{object}/fields/{field}` | Deletes a custom field and its data. |

Field definitions:

```json
{"name": "Budget__c", "type": "currency", "precision": 16, "scale": 2, "required": false}
{"name": "Stage__c", "type": "picklist", "picklistValues": ["Plan", "Build", "Run"], "restrictedPicklist": true}
{"name": "Account__c", "type": "lookup", "referenceTo": "Account", "relationshipName": "Account", "deleteRule": "SetNull"}
{"name": "Project__c", "type": "masterdetail", "referenceTo": "Project__c"}
{"name": "Ref__c", "type": "autonumber", "displayFormat": "PRJ-{0000}"}
{"name": "Margin__c", "type": "formula", "returnType": "percent", "formula": "(Amount - Cost__c) / Amount"}
{"name": "Total__c", "type": "summary", "summaryOperation": "sum",
 "summarizedObject": "Task__c", "summaryForeignKey": "Project__c", "summarizedField": "Hours__c"}
```

`type` is one of `string`, `textarea`, `email`, `phone`, `url`, `double`,
`currency`, `percent`, `boolean`, `date`, `datetime`, `picklist`,
`multipicklist`, `lookup`, `masterdetail`, `autonumber`, `formula` or
`summary`. Roll-up operations are `count`, `sum`, `min` and `max`, with an
optional `summaryFilter` formula.

### Rules

Each rule type has `GET` (list), `POST` (create) and `DELETE …/{id}`:

| Path | Rule |
| --- | --- |
| `…/tooling/validationRules` | `object`, `name`, `errorConditionFormula`, `errorMessage`, `errorDisplayField`, `active` |
| `…/tooling/workflowRules` | `object`, `name`, `criteria`, `onCreate`, `onUpdate`, `onlyWhenNewlyTrue`, `actions`, `active` |
| `…/tooling/assignmentRules` | `object` (`Lead` or `Case`), `name`, `criteria`, `userIds`, `sortOrder`, `active` |
| `…/tooling/approvalProcesses` | `object`, `name`, `entryCriteria`, `steps`, `approvedField`/`approvedValue`, `rejectedField`/`rejectedValue`, `active` |

A validation rule blocks a save when its formula evaluates to `true`.
Workflow actions:

```json
{"type": "FieldUpdate", "field": "Priority", "value": "\"High\""}
{"type": "CreateTask", "subject": "Follow up", "dueInDays": 3}
{"type": "Webhook", "webhookId": "0Wh…"}
{"type": "EmailAlert", "recipient": "owner", "subject": "Big deal closed"}
```

Assignment rules are evaluated in `sortOrder`; the first match assigns the
record round-robin across its `userIds`.

### Profiles and roles

| Request | Effect |
| --- | --- |
| `GET …/tooling/profiles` | Profiles with their object and field permissions. |
| `POST …/tooling/profiles` | Clones a profile: `{"name": …, "cloneFrom": "Standard User"}` (by name). |
| `PATCH …/tooling/profiles/{id}` | Changes object permissions (`read`, `create`, `edit`, `delete`, `viewAll`, `modifyAll`), field permissions (`readable`, `editable`) and system permissions. |
| `GET …/tooling/roles` | The role hierarchy. |
| `POST …/tooling/roles` | `{"name": "VP Sales", "parentRoleId": "00E…"}` |
| `PATCH …/tooling/roles/{id}` | Renames or re-parents a role. Cycles are rejected. |
| `DELETE …/tooling/roles/{id}` | Deletes a role with no users and no child roles. |

The standard profiles are System Administrator, Standard User, Read Only,
Marketing User and Integration User.

### Org settings

`GET`/`PATCH …/tooling/settings`:

```json
{
  "timezone": "America/New_York",
  "currency": "USD",
  "locale": "en_US",
  "currencyRates": {"EUR": 0.92},
  "apiCallsPerDay": 15000,
  "maxQueryRows": 50000,
  "recycleBinDays": 15,
  "duplicateRules": true,
  "webForms": false,
  "businessHours": {"startMinute": 540, "endMinute": 1020, "workdays": 62,
                    "slaHours": {"High": 4, "Medium": 24, "Low": 72}}
}
```

---

## Sharing

`GET`/`PUT …/tooling/sharing` reads or replaces the org's sharing model:

```json
{
  "organizationWideDefaults": {"Account": "Private", "Opportunity": "Private", "Case": "PublicRead"},
  "sharingRules": [
    {"name": "West accounts", "object": "Account", "field": "BillingState",
     "value": "CA", "roleId": "00E…", "access": "Edit"}
  ]
}
```

Org-wide defaults are `Private`, `PublicRead`, `PublicReadWrite` or
`ControlledByParent`. Access levels are `Read`, `Edit` and `All`.

A user can see a record when any of these hold:

1. The profile has *View All* for the object, or *View All Data*.
2. The org-wide default is `PublicRead` or `PublicReadWrite`.
3. The user owns the record, or someone below them in the role hierarchy does.
4. A sharing rule grants the user's role access.
5. A manual share grants the user access.
6. For `ControlledByParent` objects, the user can see the parent.

### Manual shares — `POST …/sobjects/{object}/{id}/shares`

`{"userId": "005…", "access": "Read", "reason": "Covering for Pat"}`. Only the
owner, someone above the owner in the hierarchy or an administrator can
share a record.

---

## Actions

### Convert a lead — `POST …/actions/convertLead`

```json
{
  "leadId": "00Q…",
  "convertedStatus": "Closed - Converted",
  "accountId": "001…",
  "contactId": "003…",
  "doNotCreateOpportunity": false,
  "opportunityName": "Initech - Platform",
  "ownerId": "005…"
}
```

`accountId` and `contactId` are optional: without them the lead's company and
name become a new account and contact. Returns the IDs of the account,
contact and opportunity. A converted lead can't be edited or converted again.

### Merge — `POST …/actions/merge`

`{"object": "Account", "masterId": "001…", "duplicateIds": ["001…"], "fields": {"Phone": "555-0100"}}`.
Merges one or two duplicates into the master. `fields` holds values to write
to the master first, typically the ones chosen from the duplicates. Related
records are re-parented to the master and the duplicates are deleted.
Returns the master ID, the merged IDs and how many related records moved.

### Transfer ownership — `POST …/actions/transferOwnership`

`{"object": "Account", "fromUserId": "005…", "toUserId": "005…"}`. Moves every
record of that type owned by one user to another; needs *Modify All Data*.
Returns the number transferred and the IDs that failed.

---

## Approvals

Define approval processes in the Tooling API. Each step names an `approver`:
a user ID, or `manager` for the submitter's manager.

| Request | Effect |
| --- | --- |
| `POST …/process/approvals` | `{"recordId": "006…", "comments": "Please review"}` submits a record. |
| `GET …/process/approvals` | Requests waiting on the caller. `?view=submitted` lists the caller's own submissions. |
| `POST …/process/approvals/{id}` | `{"action": "Approve" \| "Reject" \| "Recall", "comments": "…"}` |

A record is locked from the moment it is submitted until the request is
approved, rejected or recalled. On final approval or rejection the process's
field update is applied.

---

## Email templates

| Request | Effect |
| --- | --- |
| `GET …/emailTemplates` | Templates visible to the caller. |
| `POST …/emailTemplates` | `{"name": …, "object": "Contact", "subject": …, "body": …, "public": true}` |
| `DELETE …/emailTemplates/{id}` | Deletes a template. |
| `POST …/emailTemplates/{id}/render` | `{"recordId": "003…"}` returns the merged subject and body. |
| `POST …/emailTemplates/{id}/send` | Renders and queues the message in the outbox. |

Merge fields look like `{!FirstName}` or `{!Account.Name}`, with an optional
fallback: `{!Title, "there"}`.

### `GET …/outbox`

Messages queued for delivery (password resets, email alerts, template sends).
Administrators only.

---

## Reports, dashboards and forecasts

### Reports

| Request | Effect |
| --- | --- |
| `GET …/analytics/reports` | Reports the caller can see. |
| `POST …/analytics/reports` | Creates a report (below). |
| `GET …/analytics/reports/{id}` | One report definition. |
| `DELETE …/analytics/reports/{id}` | Deletes a report. |
| `POST …/analytics/reports/{id}/run` | Runs the report. `?format=csv` returns CSV. |

```json
{
  "name": "Pipeline by stage",
  "object": "Opportunity",
  "columns": ["Name", "Amount", "CloseDate"],
  "groupings": ["StageName"],
  "aggregate": "SUM(Amount)",
  "filter": "IsClosed = false",
  "sortBy": "Amount",
  "sortDescending": true,
  "rowLimit": 2000,
  "public": true
}
```

A report with `groupings` is a summary report; without, it's tabular.
Reports always run as the caller, so two users can get different numbers
from the same report.

### Dashboards

| Request | Effect |
| --- | --- |
| `GET …/analytics/dashboards` | Lists dashboards. |
| `POST …/analytics/dashboards` | `{"name": …, "runningUserId": "005…", "components": [{"title": …, "reportId": …, "display": "bar"}]}` |
| `GET …/analytics/dashboards/{id}` | One dashboard. |
| `DELETE …/analytics/dashboards/{id}` | Deletes a dashboard. |
| `POST …/analytics/dashboards/{id}/run` | Runs every component's report and returns the results. |

With `runningUserId`, every viewer sees the data as that user sees it.

### Forecasts — `GET …/forecasts?start=2026-10-01&end=2026-12-31`

Sums open and closed opportunity amounts by forecast category (`Pipeline`,
`Best Case`, `Commit`, `Closed`) for the caller and everyone below them in
the role hierarchy, broken down by owner.

---

## Bulk ingest

| Request | Effect |
| --- | --- |
| `POST …/jobs/ingest` | `{"object": "Contact", "operation": "insert"}`; `operation` is `insert`, `update`, `upsert` (with `externalIdFieldName`) or `delete`. |
| `PUT …/jobs/ingest/{id}/batches` | Uploads CSV data (`Content-Type: text/csv`). Can be called more than once. |
| `PATCH …/jobs/ingest/{id}` | `{"state": "UploadComplete"}` starts processing; `{"state": "Aborted"}` cancels. |
| `GET …/jobs/ingest` | The caller's jobs. |
| `GET …/jobs/ingest/{id}` | Job status and counters. |
| `GET …/jobs/ingest/{id}/successfulResults` | CSV: `sf__Id,sf__Created,<original columns>` |
| `GET …/jobs/ingest/{id}/failedResults` | CSV: `sf__Id,sf__Error,<original columns>` |
| `DELETE …/jobs/ingest/{id}` | Deletes a finished or aborted job. |

The first CSV row names the fields. Update and delete jobs need an `Id`
column. Rows are processed in the background through the normal record
pipeline, so validation, workflow and sharing all apply.

---

## Webhooks

Webhooks are outbound HTTP POSTs fired by workflow `Webhook` actions.

| Request | Effect |
| --- | --- |
| `GET …/webhooks` | Lists webhooks. |
| `POST …/webhooks` | `{"name": …, "url": "http://…", "secret": "…", "headers": {"X-Team": "crm"}, "active": true}` |
| `DELETE …/webhooks/{id}` | Deletes a webhook. |
| `POST …/webhooks/{id}/test` | Queues a test event. |
| `GET …/webhooks/deliveries` | Recent delivery attempts. |

Each delivery is a JSON body describing the event and the record, with
these headers:

| Header | Value |
| --- | --- |
| `X-TropiCRM-Signature` | `sha256=<hex HMAC-SHA256 of the body using the webhook secret>` |
| `X-TropiCRM-Delivery-Attempt` | Attempt number, starting at 1. |

Failed deliveries are retried with exponential backoff up to
`webhooks.max_attempts` times.

---

## Files

### Upload — `POST …/sobjects/{object}/{id}/attachments`

The request body is the file. Name it with `X-File-Name` or `?name=`;
`Content-Type` is guessed from the name if omitted. Returns the new
Attachment's ID. Attachments share their parent's visibility.

### Download — `GET …/sobjects/Attachment/{id}/body`

Returns the file with its content type.

---

## Web-to-Lead and Web-to-Case

`POST /services/web2lead` and `POST /services/web2case` accept an HTML form
post (`application/x-www-form-urlencoded`) or JSON, without authentication.
The org must enable `webForms` in its settings.

| Parameter | Meaning |
| --- | --- |
| `oid` | Org ID (query string or form field). |
| `retURL` | Where to redirect the browser afterwards. Without it the response is JSON. |
| `email`, `name` | For cases, stored as `SuppliedEmail` and `SuppliedName`. |
| any field name | Set on the new record. |

These endpoints are rate limited per client address
(`limits.public_requests_per_minute`).

---

## Audit, recent items and health

| Request | Effect |
| --- | --- |
| `GET …/setupAuditTrail?limit=100&section=Customize` | Setup changes, newest first. Needs *View Setup*. |
| `GET …/recent` | Records the caller viewed recently. |
| `GET …/tooling/healthCheck` | Security settings scored against recommended values. |
| `GET /healthz` | Liveness probe; no authentication. |
| `GET /metrics` | Request counters and latencies in Prometheus text format. |

---

## Platform administration

Operator endpoints live under `/services/admin/v1` and take
`X-Platform-Admin-Key: <key>`, where the SHA-256 of the key is configured as
`platform.admin_key_sha256` (`tropicrm-admin hash-key` prints it). Users with
*Manage Users* can call the org-scoped endpoints for their own org.

| Request | Effect |
| --- | --- |
| `GET /services/admin/v1/orgs` | All orgs with usage figures. |
| `POST /services/admin/v1/orgs` | Creates an org (same body as signup). |
| `PATCH /services/admin/v1/orgs/{orgId}` | `{"active": false}` suspends an org; also sets `name`, `apiCallsPerDay` and `storageMb`. |
| `GET /services/admin/v1/orgs/{orgId}/users` | An org's users. |
| `POST /services/admin/v1/orgs/{orgId}/users/{userId}/password` | Resets a password and clears lockout. |
| `POST /services/admin/v1/snapshot` | Snapshots every org now. |
| `GET /services/admin/v1/stats` | Server-wide counters. |
