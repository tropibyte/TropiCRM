# Formulas

Formulas compute values from a record's fields. They are used by:

* **Formula fields** — read-only fields recalculated whenever a record is read
  or saved.
* **Validation rules** — a save fails when the rule's formula is `true`.
* **Workflow rules** — the rule fires when its criteria formula is `true`;
  field-update actions set a field to the value of a formula.
* **Assignment rules, approval entry criteria and roll-up filters.**
* **Report filters** use TQL, not formulas.

```text
IF(AND(ISPICKVAL(StageName, "Closed Won"), Amount > 100000),
   "Strategic",
   CASE(Type, "New Customer", "New", "Existing Customer - Upgrade", "Upsell", "Other"))
```

---

## Syntax

### Literals

| Type | Examples |
| --- | --- |
| Number | `42`, `3.14`, `1e3` |
| Text | `"double quotes"` or `'single quotes'`; escapes `\n`, `\t`, `\r`, `\"`, `\'`, `\\` |
| Boolean | `TRUE`, `FALSE` (any case) |
| Null | `NULL` |

### Field references

Refer to fields by API name: `Amount`, `Discount__c`. Follow lookups with
dots, using the relationship name: `Account.Industry`,
`Project__r.Owner.Name`. A broken chain (empty lookup) evaluates to null.

Global values:

| Name | Value |
| --- | --- |
| `$User.Id` | The running user's ID. |
| `$User.Username` | The running user's login. |
| `$User.RoleId` | The running user's role ID. |
| `$Profile.Name` | The running user's profile name. |
| `$Organization.Id` | The org ID. |
| `$Organization.Name` | The org name. |

### Operators

From lowest to highest precedence:

| Operator | Meaning |
| --- | --- |
| `\|\|`, `OR` | Logical or. |
| `&&`, `AND` | Logical and. |
| `=`, `==`, `!=`, `<>` | Equality. Text compares case-sensitively. |
| `<`, `<=`, `>`, `>=` | Ordering for numbers, text, dates and date/times. |
| `&` | Text concatenation. Null operands become empty text. |
| `+`, `-` | Addition and subtraction. Also date arithmetic (below). |
| `*`, `/` | Multiplication and division. Dividing by zero is an error. |
| `^` | Exponentiation. |
| `-`, `!`, `NOT` | Unary minus and logical not. |

`AND`, `OR` and `NOT` work both as operators and as functions —
`AND(a, b, c)` is the same as `a && b && c`.

### Nulls

Arithmetic with a null operand yields null. `=` treats null and empty text
as equal to each other and to nothing else; `<`, `<=`, `>` and `>=` are false
when either side is null. In a boolean context (an `IF` condition, a
validation rule), null counts as false. Use `ISBLANK`, `BLANKVALUE` and
`NULLVALUE` to handle missing values explicitly.

### Dates

| Expression | Result |
| --- | --- |
| `date + n`, `date - n` | A date `n` days later or earlier. |
| `date - date` | Number of days between. |
| `datetime + n` | `n` may be fractional: `NOW() + 0.5` is twelve hours from now. |
| `datetime - datetime` | Days between, as a decimal. |

`TODAY()` and date conversions use the org's time zone.

---

## Functions

### Logic

| Function | Description |
| --- | --- |
| `IF(cond, then, else)` | Only the chosen branch is evaluated. |
| `CASE(expr, v1, r1, v2, r2, …, else)` | Returns the result paired with the first value equal to `expr`, or `else`. |
| `AND(a, b, …)`, `OR(a, b, …)`, `NOT(a)` | Boolean logic; `AND` and `OR` short-circuit. |
| `ISBLANK(x)`, `ISNULL(x)` | `TRUE` for null or empty text. |
| `BLANKVALUE(x, fallback)`, `NULLVALUE(x, fallback)` | `x`, or `fallback` when `x` is blank. |
| `ISNUMBER(text)` | `TRUE` if the text parses as a number. |

### Text

| Function | Description |
| --- | --- |
| `LEN(text)` | Length. |
| `LEFT(text, n)`, `RIGHT(text, n)` | The first or last `n` characters. |
| `MID(text, start, n)` | `n` characters starting at `start` (1-based). |
| `UPPER(text)`, `LOWER(text)`, `TRIM(text)` | Case conversion and whitespace trimming. |
| `CONTAINS(text, part)` | Case-sensitive substring test. |
| `BEGINS(text, prefix)` | Case-sensitive prefix test. |
| `FIND(part, text [, start])` | 1-based position of `part` in `text`, or 0. |
| `SUBSTITUTE(text, old, new)` | Replaces every occurrence. |
| `LPAD(text, width [, pad])`, `RPAD(…)` | Pads (or truncates) to `width` characters. `pad` defaults to a space. |
| `TEXT(value)` | Converts a number, date, date/time or boolean to text. |
| `VALUE(text)` | Parses a number; an error if the text isn't numeric. |
| `REGEX(text, pattern)` | `TRUE` if the whole text matches the ECMAScript regular expression. |
| `HYPERLINK(url, label)` | Display text for a link. |
| `IMAGE(url, alt [, height])` | Display text for an image (the alt text). |
| `BR()` | A line break. |

### Math

| Function | Description |
| --- | --- |
| `ROUND(n, digits)` | Rounds half away from zero. Negative `digits` round to tens, hundreds, … |
| `FLOOR(n)`, `CEILING(n)` | Round down or up to a whole number. |
| `ABS(n)` | Absolute value. |
| `MAX(a, b, …)`, `MIN(a, b, …)` | Ignore null arguments; null if all are null. |
| `MOD(n, d)` | Remainder with the sign of `n`. `d` must not be zero. |
| `SQRT(n)` | Square root; an error for negative numbers. |

### Dates

| Function | Description |
| --- | --- |
| `TODAY()` | Today's date in the org's time zone. |
| `NOW()` | The current date/time. |
| `DATE(year, month, day)` | Builds a date. |
| `DATEVALUE(x)` | Converts text (`YYYY-MM-DD`) or a date/time to a date. |
| `DATETIMEVALUE(x)` | Converts text or a date to a date/time (midnight UTC for dates). |
| `YEAR(d)`, `MONTH(d)`, `DAY(d)` | Date parts. |
| `WEEKDAY(d)` | 1 = Sunday … 7 = Saturday. |
| `ADDMONTHS(d, n)` | Adds months, clamping to the last day of shorter months (`2026-01-31` + 1 → `2026-02-28`). |

### Picklists

| Function | Description |
| --- | --- |
| `ISPICKVAL(field, "value")` | `TRUE` if the picklist field has that value. |
| `INCLUDES(field, "value")` | `TRUE` if the multi-select picklist has that value selected. |

### Change tracking

Available in validation rules, workflow rules and field-update formulas:

| Function | Description |
| --- | --- |
| `ISNEW()` | `TRUE` while a record is being created. |
| `ISCHANGED(field)` | `TRUE` if the save changes the field. Always `FALSE` on create. |
| `PRIORVALUE(field)` | The field's value before this save; null on create. |

---

## Formula field return types

A formula field's `returnType` decides how its result is stored and shown:

| `returnType` | Result |
| --- | --- |
| `string` | Text, truncated to 3,900 characters. |
| `double`, `currency`, `percent` | Numbers, rounded to the field's `scale`. |
| `boolean` | Checkbox. |
| `date`, `datetime` | Dates. |

A result of the wrong type for the field (text from a `double` formula, say)
leaves the field null. An evaluation error — division by zero, `VALUE()` of
non-numeric text, `SQRT()` of a negative number — fails the request with
`FORMULA_EVALUATION_ERROR`.

---

## Examples

Days a case has been open:

```text
IF(IsClosed, ClosedDate - CreatedDate, NOW() - CreatedDate)
```

Validation: discounts over 20% need a reason:

```text
AND(Discount__c > 0.2, ISBLANK(Discount_Reason__c))
```

Validation: the close date can't move into the past:

```text
AND(ISCHANGED(CloseDate), CloseDate < TODAY())
```

Workflow criteria: the stage just moved to Closed Won:

```text
AND(ISCHANGED(StageName), ISPICKVAL(StageName, "Closed Won"))
```

Display name with a fallback:

```text
BLANKVALUE(Nickname__c, FirstName) & " " & UPPER(LEFT(LastName, 1)) & "."
```
