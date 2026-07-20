# Field Coverage

This page summarises which fields MppIO decodes and which areas the native MPP14 writer regenerates.
Every entry marked **read** is validated in the test suite against Microsoft Project's own XML export
of the same file across several real fixtures. Binary write coverage is also exercised by semantic
round trips and raw-stream tests.

Schedule fields such as dates, work, actuals, costs, calendars, availability, baselines, and custom
fields are also read and written as Microsoft Project compatible XML by
[`XmlIO`](../API/XmlIO.md). MPP view/row/cell formatting is binary-only and is not represented by
the current MSPDI serializer. `XmlIO` reading a `.xml` is cross-checked against `MppIO` reading the
matching `.mpp`. See [XML Interchange](../GettingStarted/XmlInterchange.md).

## Project

| Field | Status |
| :--- | :--- |
| Title, Author | read / MPP14 write |
| Start date, Finish date, Status date | read / MPP14 write |
| Default project calendar | read / MPP14 write |
| Format version | read; selects writer on save |
| View text, line, and standard bar styles | read / MPP14 write for supported views/categories |
| Report accent color | model/internal scaffold; MPP and MSPDI persistence not yet |

## Tasks

| Field | Status |
| :--- | :--- |
| Unique ID, ID | read |
| Name | read |
| WBS | read (derived from the outline) |
| Outline level | read |
| Start, Finish | read (automatic **and** manual scheduling) |
| Duration | read |
| Percent complete | read (leaf tasks exact; summary tasks use Microsoft Project's rolled-up value) |
| Milestone | read |
| Summary | read (derived from the outline) |
| Constraint type, Constraint date | read |
| Cost, Fixed/Actual/Remaining cost, Cost variance | read |
| Baselines 0–10 (cost, work, start, finish, duration) | read |
| Custom fields (Text/Number/Cost/Date/Duration/Start/Finish/Flag/Outline Code) | read |
| Notes | read (raw RTF source) |
| Manual/automatic mode, task type, effort-driven, priority, deadline | read / MPP14 write |
| Task work, actual dates/duration/work, earned-value metrics | read / MPP14 write |
| Active/inactive state | read / MPP14 write |
| Row and per-cell font/color/background formatting | read / MPP14 write |
| Per-task bar color | model/internal scaffold; MPP and MSPDI persistence not yet |
| Critical, late dates, total/free slack | computed by `Scheduler`; stored-field coverage varies |

## Resources

| Field | Status |
| :--- | :--- |
| Unique ID, ID | read |
| Name, Initials | read |
| Max units | read |
| Cost, Actual/Remaining cost, Cost variance | read (stored value, else rolled up from assignments) |
| Baselines 0–10 (cost, work) | read |
| Custom fields | read |
| Notes | read (raw RTF source) |
| Cost-rate tables A–E (standard/overtime rate, cost-per-use, effective dates) | read |
| Resource calendar | read / MPP14 write (via a derived per-resource calendar row) |
| Availability table (time-phased max units) | read / MPP14 write |
| Group, code, e-mail, type | not yet |

## Assignments

| Field | Status |
| :--- | :--- |
| Unique ID | read |
| Task / Resource unique id | read |
| Units | read |
| Work | read |
| Start, finish, assignment/leveling delay | read / MPP14 write |
| Actual and remaining work | read / MPP14 write |
| Cost, Actual/Remaining cost, Cost variance | read |
| Baselines 0–10 (cost, work, start, finish) | read |
| Custom fields | read |
| Notes | read (raw RTF source) |

## Relations (predecessor links)

| Field | Status |
| :--- | :--- |
| Predecessor / successor task | read |
| Link type | read |
| Lag | read |
| Lag display unit | read / MPP14 write |

## Calendars

| Field | Status |
| :--- | :--- |
| Unique ID | read |
| Name | read |
| Base calendar | read |
| Working-day mask | read |
| Working hours (per weekday) | read |
| Exceptions (holidays / one-off days) | read |

## Save preservation rule

The MPP14 writer starts with ScheduleIO's embedded template and regenerates modeled entity and view
data. This is not an in-place patch of the source file. Unsupported fields, custom views, macros, and
other unmodeled source-only content should not be assumed to survive a read/edit/write cycle. See
[MPP File Format and Storage](FileFormat.md) for the exact pipeline.

## Validation approach

The decoder is checked against ground truth rather than only round-tripping itself:

* **Unit tests** exercise the low-level codecs (compound-file container, record streams, field
  decoders) on synthetic data.
* **Oracle tests** open real `.mpp` fixtures and compare the decoded model against the matching
  Microsoft Project `.xml` export — task names, dates, durations, IDs, flags, resources,
  assignments, predecessor links, calendars, **cost** values, **baselines** (matched per baseline
  number), **custom fields** (matched by field id against the XML `<ExtendedAttribute>` values), and
  **notes** (XML plain text recovered from the decoded RTF), and resource **cost-rate tables**
  (current rate and the full set of time-phased standard rates per resource), and calendar
  **working hours** (per weekday) and **exceptions** (holidays / one-off working days).

Where a `.mpp` legitimately contains more rows than a filtered XML export (extra assignment or task
records, for example), the oracle measures **recall** of the exported items rather than requiring an
exact one-to-one match.

**Notes** are stored by Microsoft Project as raw RTF and exported to XML as plain text, so the notes
oracle checks that each line of the XML plain-text note is recovered inside the decoded RTF. Task and
resource notes are validated this way against a fixture that contains them; assignment notes use the
same decoder but are currently covered only by the read→write→read round-trip (no fixture has them).
