# Field Coverage

This page summarises which fields MppIO decodes today. Every entry marked **read** is validated in
the test suite against Microsoft Project's own XML export of the same file, across several real
fixtures.

Every field listed here is also read **and written** as Microsoft Project compatible XML by
[`XmlIO`](../API/XmlIO.md): the XML round-trip (read → write → read) is verified to be lossless for
all of them on the same fixtures, and `XmlIO` reading a `.xml` is cross-checked against `MppIO`
reading the matching `.mpp`. See [XML Interchange](../GettingStarted/XmlInterchange.md).

## Project

| Field | Status |
| :--- | :--- |
| Title, Author | read (from the document summary information) |
| Start date, Finish date | read |
| Format version | read |

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
| Critical, slack | not yet (derived from the schedule) |

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
| Group, code, e-mail, type | not yet |

## Assignments

| Field | Status |
| :--- | :--- |
| Unique ID | read |
| Task / Resource unique id | read |
| Units | read |
| Work | read |
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

## Calendars

| Field | Status |
| :--- | :--- |
| Unique ID | read |
| Name | read |
| Base calendar | read |
| Working-day mask | read |
| Working hours (per weekday) | read |
| Exceptions (holidays / one-off days) | read |

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
