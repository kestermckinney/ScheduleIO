# schedule::TimephasedValue

One dated assignment bucket from MSPDI `<TimephasedData>` or a supported native MPP14 time-phased
stream. `Assignment::timephasedValues` stores the buckets.

```cpp
#include "model/timephasedvalue.h"
```

## Members

| Member | Type | Default | Meaning |
| :--- | :--- | :--- | :--- |
| `type` | `int` | `0` | Microsoft/assignment bucket type; known values are listed below. Unknown values can still round-trip through XML. |
| `uniqueId` | `int` | `0` | Bucket identifier; this is not the assignment UID. |
| `start` | `QDateTime` | invalid | Inclusive bucket start. |
| `finish` | `QDateTime` | invalid | Exclusive bucket finish for interval calculations. |
| `unit` | `int` | `0` | MSPDI time unit/frequency code. Native ScheduleIO-generated daily buckets use `1`. |
| `baselineNumber` | `int` | `0` | Baseline slot for baseline types; `0` is Baseline, `1`–`10` are Baseline1–Baseline10. |
| `value` | `QString` | empty | Original XML-form value, retained as text to avoid losing unknown-type precision or semantics. |

## Known assignment types

| Enumerator | Code | Value form |
| :--- | :--- | :--- |
| `RemainingWork` | `1` | ISO 8601 duration |
| `ActualWork` | `2` | ISO 8601 duration |
| `ActualOvertimeWork` | `3` | ISO 8601 duration |
| `BaselineWork` | `4` | ISO 8601 duration; uses `baselineNumber` |
| `BaselineCost` | `5` | Decimal currency amount; uses `baselineNumber` |
| `ActualCost` | `6` | Decimal currency amount |

Work values use XML duration text such as `PT8H` or `PT1H30M`. Cost values use decimal text. The raw
string is authoritative for serialization.

## Conversion helpers

| Method | Result |
| :--- | :--- |
| `durationMillis()` | Parses a signed ISO duration containing days/hours/minutes/seconds; invalid text returns `0`. Days are 24 elapsed hours in this XML representation. |
| `durationInPeriod(from, to)` | Prorates the parsed duration over the overlap with half-open range `[from, to)`. |
| `amount()` | Parses `value` as `double`; invalid text returns `0.0`. |
| `amountInPeriod(from, to)` | Prorates the amount over the overlap with `[from, to)`. |

Overlap helpers prorate by wall-clock span. They return zero for invalid/reversed bucket dates or an
empty query interval.

## Authority and coverage

When actual- or remaining-work buckets exist, `ProjectReconciliation` treats that stream as
authoritative for its aggregate assignment field. Assignment setters can replace an interval while
preserving surrounding bucket fragments.

Native MPP14 supports remaining work, actual work, actual overtime work, baseline work, and baseline
cost through the current codec. MSPDI preserves the complete modeled row and can retain unknown type
codes. Consult [Field Coverage](../Reference/FieldCoverage.md) for current format boundaries.
