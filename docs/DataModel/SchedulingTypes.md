# Scheduling Helper Types

Most scheduling utilities expose static functions, but several public configuration and result
structures are useful to applications. They operate on the value model and perform no file I/O.

## schedule::ResourceLeveling::Options

```cpp
#include "model/resourceleveling.h"
```

| Member | Type | Default | Meaning |
| :--- | :--- | :--- | :--- |
| `order` | `ResourceLeveling::Order` | `PriorityStandard` | Candidate ordering: `IdOnly`, `Standard`, or `PriorityStandard`. |
| `levelOnlyWithinAvailableSlack` | `bool` | `false` | Refuse delays beyond each task's available slack. |
| `allowTaskSplitting` | `bool` | `true` | Permit splitting where the leveling implementation supports it. |
| `taskUniqueIds` | `QSet<int>` | empty | Tasks eligible to receive delay; empty means all. Other tasks still contribute load. |

## schedule::ResourceLeveling::Overallocation

One half-open time window during which demand exceeds a resource's capacity:

| Member | Type | Meaning |
| :--- | :--- | :--- |
| `resourceUniqueId` | `int` | Conflicted resource UID. |
| `start`, `finish` | `QDateTime` | Conflict window. |
| `peakUnits` | `double` | Greatest concurrent assignment-unit demand in the window. |
| `maxUnits` | `double` | Available capacity for the window. |

`ResourceLeveling::overallocations()` returns these windows by resource in time order.

## schedule::ResourceLeveling::WorkProfile

A cached sampling context for repeatedly asking how much of one assignment's work falls in date
columns:

| Member | Meaning |
| :--- | :--- |
| `assignment` | Non-owning pointer to the sampled assignment; the source project/container must outlive the profile and must not reallocate it. |
| `timephased` | Authoritative buckets exist; span-distribution fields are then unused. |
| `start`, `finish` | Effective assignment/task span. |
| `calendar` | Resolved task/resource-intersection `WorkCalendar`. |
| `spanWork` | Working milliseconds in the complete span. |
| `taskCalendarUid`, `resourceCalendarUid` | Cache keys identifying the resolved calendar inputs; resource UID is `-1` when none participates. |

`sameCalendarAs()` compares those two calendar keys. Build once with `workProfile()` and call the
profile overload of `workInPeriod()` for many columns.

## schedule::ProjectReconciliation::Totals

```cpp
#include "model/projectreconciliation.h"
```

| Member | Type | Meaning |
| :--- | :--- | :--- |
| `workMillis`, `actualWorkMillis`, `remainingWorkMillis` | `qint64` | Total, completed, and remaining regular work. |
| `overtimeWorkMillis`, `actualOvertimeWorkMillis`, `remainingOvertimeWorkMillis` | `qint64` | Overtime subsets. |
| `cost`, `actualCost`, `remainingCost` | `double` | Total, incurred, and remaining currency amounts. |
| `overtimeCost` | `double` | Overtime cost included in the totals. |

`taskTotals()` and `resourceTotals()` return computed values without mutating the project;
`reconcile()` writes canonical aggregates and rollups.

## schedule::ProgressUpdating::UpdateAction

`ZeroOrOneHundred` records progress only for tasks finishing on or before the boundary.
`ScheduledPercent` records the scheduled duration/work completed through the boundary. Both are used
by `updateScheduledProgress()`.

## schedule::WorkContouring::Contour

The assignment contour codes are `Flat = 0`, `BackLoaded = 1`, `FrontLoaded = 2`,
`DoublePeak = 3`, `EarlyPeak = 4`, `LatePeak = 5`, `Bell = 6`, `Turtle = 7`, and
`Contoured = 8`. `Contoured` means custom time-phased work rather than a generated preset.

## schedule::WorkCalendar helper structures

`WorkCalendar::Period` stores a resolved period as millisecond-of-day `begin` and `end` values.
`WorkCalendar::Exception` is the resolved equivalent of `CalendarException`: date bounds, working
flag, resolved periods, recurrence fields, and occurrence limits. These are public for deterministic
recurrence helpers; normal callers should construct `Calendar`, `TimeRange`, and `CalendarException`
values, then query a `WorkCalendar` through `workingTimes()`, `addWork()`, and `workBetween()`.

See [Scheduling Utilities](../API/Scheduling.md) for operations and recommended call order.
