# schedule::Assignment

Links a resource to a task. Value type; copyable and equality-comparable.

```cpp
#include "src/model/assignment.h"
```

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `uniqueId` | `int` | The assignment's own unique id. |
| `taskUniqueId` | `int` | Unique id of the assigned [`schedule::Task`](Task.md). |
| `resourceUniqueId` | `int` | Unique id of the assigned [`schedule::Resource`](Resource.md). |
| `budget` | `bool` | Project-summary budget assignment. |
| `budgetCost` | `double` | Budget cost carried by the assignment. |
| `budgetWorkMillis` | `qint64` | Budget work carried by the assignment, in milliseconds. |
| `units` | `double` | Assigned units, as a ratio. `1.0` == 100%. |
| `workMillis` | `qint64` | Assigned work, in milliseconds. |
| `start`, `finish` | `QDateTime` | Scheduled assignment span; invalid means use the task span. |
| `stop`, `resume` | `QDateTime` | Boundary between completed and remaining work for a partially progressed assignment. |
| `delayMillis` | `qint64` | Assignment delay in working milliseconds. |
| `levelingDelayMillis` | `qint64` | Delay added by resource leveling. |
| `actualWorkMillis` | `qint64` | Work completed. |
| `remainingWorkMillis` | `qint64` | Work still scheduled. |
| `overtimeWorkMillis`, `actualOvertimeWorkMillis`, `remainingOvertimeWorkMillis` | `qint64` | Overtime subsets of total, actual, and remaining work. |
| `timephasedValues` | `QList<schedule::TimephasedValue>` | Dated assignment values. Planned/remaining, actual, and actual overtime work round-trip through MSPDI and native MPP14. |
| `costRateTable` | `int` | Selected resource cost-rate table A–E (`0`–`4`). |
| `variableRateUnits` | `int` | Material consumption denominator; `0` is fixed, otherwise Microsoft minute/hour/day/week/month/year code. |
| `workContour` | `int` | Assignment work distribution: `0` Flat through `7` Turtle; `8` is Contoured/custom. |
| `notes` | `QString` | The assignment's notes as raw RTF source (empty if none). |
| `cost` | `double` | Total cost, in the project's currency unit. |
| `actualCost` | `double` | Cost incurred so far. |
| `remainingCost` | `double` | Cost still to be incurred. |
| `overtimeCost`, `actualOvertimeCost`, `remainingOvertimeCost` | `double` | Cost of overtime work at the resource's applicable overtime rate. |
| `costVariance` | `double` | Cost minus baseline cost. |
| `baselines` | `QList<schedule::Baseline>` | Saved baselines (cost, work, start, finish; see [`schedule::Baseline`](Baseline.md)). |
| `customFields` | `QList<schedule::CustomField>` | Populated custom/extended fields (see [`schedule::CustomField`](CustomField.md)). |

## Time-phased helper methods

The interval helpers use half-open ranges `[from, to)`. Setters replace the selected interval while
preserving bucket portions outside it; getters return the prorated overlap:

| Method family | Value |
| :--- | :--- |
| `setTimephasedWorkInPeriod()` / `timephasedWorkInPeriod()` | Work milliseconds for a bucket type. |
| `setTimephasedCostInPeriod()` / `timephasedCostInPeriod()` | Currency amount for a type and baseline number. |
| `setTimephasedMaterialInPeriod()` / `timephasedMaterialInPeriod()` | Material quantity, hiding MPP's duration-hour storage convention. |

See [TimephasedValue](TimephasedValue.md) for bucket fields and type codes.

## Resolving the links

An assignment references its task and resource by unique id. Join them with lookups:

```cpp
QHash<int, const schedule::Task*>     taskById;
QHash<int, const schedule::Resource*> resById;
for (const schedule::Task &t : project.tasks)     taskById.insert(t.uniqueId, &t);
for (const schedule::Resource &r : project.resources) resById.insert(r.uniqueId, &r);

for (const schedule::Assignment &a : project.assignments) {
    const schedule::Task     *t = taskById.value(a.taskUniqueId);
    const schedule::Resource *r = resById.value(a.resourceUniqueId);
    if (t && r)
        qInfo() << r->name << "->" << t->name
                << "units" << a.units
                << "work(h)" << a.workMillis / 3600000.0;
}
```

## Notes

* A `.mpp` file can contain more assignment records than appear in a filtered Microsoft Project
  export (deleted or internal rows). MppIO returns the records it finds; filter by valid
  `taskUniqueId` / `resourceUniqueId` if you only want live assignments.
* Native MPP14 maps regular actual work, remaining/planned work, and actual overtime work.
  Time-phased cost and baseline streams are not yet decoded or written.
* `setTimephasedWorkInPeriod()` replaces one actual/remaining-work interval, preserves bucket
  portions outside it, and reconciles aggregate assignment work values for usage-grid editing.
* `setTimephasedMaterialInPeriod()` and `timephasedMaterialInPeriod()` expose Microsoft Project's
  material convention: one duration-hour in the assignment stream represents one material unit.
  This keeps fractional, dated quantities compatible with MSPDI and native MPP while callers work
  in real quantities.
* `MaterialCosting::recalculate()` applies the selected material standard rate and cost-per-use,
  splits actual versus remaining cost from the quantity buckets, and rolls costs up to tasks and
  resources.
* `WorkContouring` expands the predefined contours across ten working-time segments and writes
  authoritative remaining-work buckets. Custom contour buckets are retained exactly. Native MPP14
  stores predefined identity in the remaining-work stream header and the custom state in assignment
  metadata.
* Assignment `start` and `finish` are normally inside the linked task span. Invalid values mean use
  the task dates. The scheduling invariant is assignment start = task start + assignment delay +
  leveling delay, measured on the applicable working calendar.
