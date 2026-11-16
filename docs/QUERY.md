# TQL — TropiCRM Query Language

TQL is a read-only query language modelled on SOQL. A query names one
object, the fields to return and optional filters, grouping, ordering and
paging:

```sql
SELECT Name, Industry, Owner.Name,
       (SELECT LastName, Email FROM Contacts WHERE Email != null)
FROM Account
WHERE Industry IN ('Technology', 'Finance') AND CreatedDate = LAST_N_DAYS:30
ORDER BY Name ASC NULLS LAST
LIMIT 50 OFFSET 100
```

Run queries with `GET /services/data/v1/query?q=…` (or `queryAll` to include
deleted records). Keywords, object names and field names are
case-insensitive.

Every query runs as the calling user: records they can't see are skipped,
and fields hidden by field-level security can't be selected, filtered or
sorted on.

---

## SELECT

The select list is a comma-separated mix of:

| Item | Example |
| --- | --- |
| A field | `Name` |
| A parent field | `Account.Owner.Name` |
| A child subquery | `(SELECT Subject FROM Tasks)` |
| An aggregate | `COUNT()`, `SUM(Amount) total` |

Each row carries an `attributes` object with the object type and the record's
URL. Select `Id` to get the ID as a field.

### Parent relationships

Follow a lookup with the relationship name (the field name without `Id`, or
with `__r` instead of `__c` for custom lookups):

```sql
SELECT Name, Account.Name, Account.Owner.Name FROM Contact
SELECT Name, Project__r.Name, Project__r.Account__r.Name FROM Task__c
```

Paths can be up to five relationships deep. Polymorphic lookups such as
`WhatId`, `WhoId` and `ParentId` only support `Id` and `Name` on the parent
(`What.Name`).

Parent fields come back as nested objects:

```json
{"attributes": {…}, "Id": "003…", "Name": "Hank Scorpio", "Account": {"Name": "Globex", "Owner": {"Name": "Ada Admin"}}}
```

When the lookup is empty the relationship is `null`.

### Child subqueries

Use the child relationship name, which is the plural label without spaces
for standard objects (`Contacts`, `Opportunities`, `Tasks`) and
`<Object>s__r` for custom objects (`Task__c` → `Tasks__r`). The
`childRelationships` section of `describe` lists them.

```sql
SELECT Name, (SELECT Name, Amount FROM Opportunities WHERE IsClosed = false ORDER BY Amount DESC LIMIT 5)
FROM Account
```

Subqueries support `WHERE`, `ORDER BY` and `LIMIT`. Each parent row gets a
nested result:

```json
"Opportunities": {"totalSize": 2, "done": true, "records": [ … ]}
```

or `null` when there are no children.

---

## WHERE

Conditions combine with `AND`, `OR`, `NOT` and parentheses. `AND` binds
tighter than `OR`.

| Operator | Meaning |
| --- | --- |
| `=` `!=` `<>` | Equal / not equal. String comparison is case-insensitive. |
| `<` `<=` `>` `>=` | Ordering for numbers, dates, date/times and strings. |
| `LIKE 'pat%'` | `%` matches any run of characters and `_` one character. Case-insensitive. |
| `NOT LIKE` | Negated `LIKE`. |
| `IN (a, b)` | Matches any listed value. |
| `NOT IN (a, b)` | Matches none of the listed values. |
| `INCLUDES ('a', 'b;c')` | Multi-select picklists: any listed item is selected. `'b;c'` means both `b` and `c`. |
| `EXCLUDES ('a')` | Multi-select picklists: none of the listed items is selected. |

```sql
SELECT Name FROM Lead WHERE (Status = 'Open' OR Status = 'Working') AND NOT Company LIKE 'Test%'
SELECT Name FROM Account WHERE Id IN ('001…', '001…')
```

### Literals

| Type | Syntax |
| --- | --- |
| String | `'O\'Reilly'` — single quotes; escapes `\'`, `\\`, `\n`, `\r`, `\t`, plus `\%` and `\_` for literal wildcards in `LIKE` |
| Number | `42`, `-3.5`, `1e6` |
| Boolean | `true`, `false` |
| Null | `null` — use `= null` and `!= null` |
| Date | `2026-11-16` |
| Date/time | `2026-11-16T09:30:00Z`, `2026-11-16T09:30:00-05:00` |

Comparing a date/time field with a date literal compares against the start
of that day in the org's time zone.

### Relative dates

Relative date literals are evaluated in the org's time zone and stand for a
range of days. `=` matches anything in the range, `<` means before it starts
and `>` means after it ends.

| Literal | Range |
| --- | --- |
| `YESTERDAY`, `TODAY`, `TOMORROW` | One day. |
| `LAST_WEEK`, `THIS_WEEK`, `NEXT_WEEK` | Sunday to Saturday. |
| `LAST_MONTH`, `THIS_MONTH`, `NEXT_MONTH` | Calendar months. |
| `LAST_QUARTER`, `THIS_QUARTER`, `NEXT_QUARTER` | Calendar quarters starting in January, April, July and October. |
| `LAST_YEAR`, `THIS_YEAR`, `NEXT_YEAR` | Calendar years. |
| `LAST_90_DAYS` | The 90 days before today, and today. |
| `NEXT_90_DAYS` | Today and the next 90 days. |
| `LAST_N_DAYS:n` | The `n` days before today, and today. |
| `NEXT_N_DAYS:n` | Today and the next `n` days. |
| `N_DAYS_AGO:n` | The single day `n` days ago. |
| `LAST_N_MONTHS:n` | The `n` whole months before this one. |
| `NEXT_N_MONTHS:n` | The `n` whole months after this one. |

```sql
SELECT Subject FROM Task WHERE ActivityDate < TODAY AND IsClosed = false
SELECT Name FROM Opportunity WHERE CloseDate = NEXT_N_DAYS:30
```

`n` must be a non-negative integer.

---

## Aggregates and GROUP BY

| Function | Result |
| --- | --- |
| `COUNT()` | Number of matching rows. On its own, the result is just `totalSize`. |
| `COUNT(field)` | Rows where the field isn't null. |
| `COUNT_DISTINCT(field)` | Distinct non-null values. |
| `SUM(field)`, `AVG(field)` | Numeric fields only. |
| `MIN(field)`, `MAX(field)` | Numbers, dates, date/times and strings. |

```sql
SELECT COUNT() FROM Contact WHERE Account.Industry = 'Technology'

SELECT StageName, COUNT(Id) deals, SUM(Amount) pipeline
FROM Opportunity
WHERE IsClosed = false
GROUP BY StageName
HAVING SUM(Amount) > 100000
ORDER BY SUM(Amount) DESC
```

Every selected field that isn't aggregated must appear in `GROUP BY`. Give
an aggregate an alias by writing a name after it; otherwise the results are
named `expr0`, `expr1`, … in select order. Grouped results have no `Id` and
come back as `AggregateResult` rows:

```json
{"attributes": {"type": "AggregateResult"}, "StageName": "Negotiation", "deals": 4, "pipeline": 845000}
```

`HAVING` filters groups and can refer to aggregates or grouped fields.

---

## ORDER BY, LIMIT and OFFSET

```sql
ORDER BY Account.Name ASC, CloseDate DESC NULLS LAST
```

* `ASC` is the default.
* Nulls sort first in ascending order and last in descending order unless
  `NULLS FIRST` or `NULLS LAST` says otherwise.
* Ties keep a stable order by record ID.
* `LIMIT n` caps the number of rows.
* `OFFSET n` skips rows (at most 2,000); use cursors to page deeper.

---

## Deleted records

`queryAll` includes records in the recycle bin. You can also add
`ALL ROWS` at the end of a query:

```sql
SELECT Name, IsDeleted FROM Account WHERE IsDeleted = true ALL ROWS
```

---

## Paging

Large results are split into batches (2,000 rows by default; set
`Sforce-Query-Options: batchSize=N` for 200–2,000). When `done` is `false`,
`GET` the `nextRecordsUrl` to fetch the next batch. Cursors expire after
fifteen minutes and belong to the user who ran the query.

---

## Grammar

```
query        := SELECT select_list FROM object
                [WHERE condition]
                [GROUP BY path {, path}]
                [HAVING condition]
                [ORDER BY order_item {, order_item}]
                [LIMIT int] [OFFSET int] [ALL ROWS]

select_list  := select_item {, select_item}
select_item  := path
              | aggregate [alias]
              | ( SELECT path {, path} FROM relationship
                  [WHERE condition] [ORDER BY order_item {, order_item}] [LIMIT int] )

aggregate    := COUNT() | (COUNT | COUNT_DISTINCT | SUM | AVG | MIN | MAX) ( path )
path         := name {. name}

condition    := and_expr {OR and_expr}
and_expr     := not_expr {AND not_expr}
not_expr     := [NOT] primary
primary      := ( condition ) | comparison
comparison   := operand_lhs op value
              | operand_lhs [NOT] IN ( value {, value} )
              | operand_lhs [NOT] LIKE string
              | operand_lhs (INCLUDES | EXCLUDES) ( value {, value} )
operand_lhs  := path | aggregate
op           := = | != | <> | < | <= | > | >=
value        := string | number | true | false | null | date | datetime
              | date_literal | date_literal:int

order_item   := (path | aggregate) [ASC | DESC] [NULLS (FIRST | LAST)]
```
