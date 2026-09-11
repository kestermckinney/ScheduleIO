# schedule::Resource

A resource (a person, piece of equipment, or material that work is assigned to). Value type;
copyable and equality-comparable.

```cpp
#include "src/model/resource.h"
```

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `uniqueId` | `int` | Stable identity. Referenced by assignments (`resourceUniqueId`) and by resource calendars. |
| `id` | `int` | Display / sheet position (the resource's ID column). |
| `name` | `QString` | Resource name. |
| `initials` | `QString` | Resource initials. |
| `type` | `schedule::Resource::Type` | `Work`, `Material`, or `Cost`; defaults to `Work`. |
| `budget` | `bool` | Budget resource intended for assignment to the project summary. |
| `materialLabel` | `QString` | Unit label for a material resource, such as `tons` or `boxes`. |
| `maxUnits` | `double` | Maximum units available, as a ratio. `1.0` == 100%. |
| `calendarUniqueId` | `int` | Resource calendar UID, or `-1` when none is recorded. |
| `notes` | `QString` | The resource's notes as raw RTF source (empty if none). |
| `cost` | `double` | Total cost, in the project's currency unit (rolled up from assignments when not stored). |
| `actualCost` | `double` | Cost incurred so far. |
| `remainingCost` | `double` | Cost still to be incurred. |
| `costVariance` | `double` | Cost minus baseline cost. |
| `budgetCost` | `double` | Budget-resource cost amount. |
| `budgetWorkMillis` | `qint64` | Budget-resource work amount in milliseconds. |
| `baselines` | `QList<schedule::Baseline>` | Saved baselines (cost and work only; see [`schedule::Baseline`](Baseline.md)). |
| `customFields` | `QList<schedule::CustomField>` | Populated custom/extended fields (see [`schedule::CustomField`](CustomField.md)). |
| `costRates` | `QList<schedule::CostRate>` | Cost-rate tables A–E with time-phased rates (see [`schedule::CostRate`](CostRate.md)). |
| `availabilityTable` | `QList<schedule::AvailabilityPeriod>` | Time-phased maximum-unit rows; invalid endpoints mean an open range. |

## Notes

* Resources are linked to tasks through [`schedule::Assignment`](Assignment.md), not directly.
* `Resource::Type` is an `enum class`: `Material = 0`, `Work = 1`, and `Cost = 2`. Work resources
  contribute labor and calendar capacity; material resources represent quantities; cost resources
  contribute an entered cost without work.
* `maxUnits` and dated [availability](Availability.md) apply to work-resource capacity. A material
  assignment's `units` represent quantity/rate instead, and a cost resource has no labor capacity.
* A resource may also have a *resource calendar* — a [`schedule::Calendar`](Calendar.md) whose name is
  the resource's name and whose `baseCalendarUniqueId` points at a base calendar.
* Work-resource calendars constrain assignment working intervals. `availabilityTable` changes dated
  capacity for over-allocation and leveling; like Microsoft Project, availability alone does not move
  a task until leveling is requested.
* Resource **cost rates** (standard rate, overtime rate, cost-per-use) live in Microsoft Project's
  cost-rate tables and are decoded into `costRates` (see [`schedule::CostRate`](CostRate.md)).
* **`cost`** is the resource's stored cost where present; for resources whose cost Microsoft Project
  computes from rate tables, MppIO rolls it up from the resource's assignment costs so it matches the
  exported value.
* Budget fields are distinct from ordinary cost/work totals and are normally used only with budget
  resources and project-summary assignments.

## Example

```cpp
for (const schedule::Resource &r : project.resources)
    qInfo() << r.id << r.name << "(" << r.initials << ")"
            << "max" << int(r.maxUnits * 100) << "%";
```
