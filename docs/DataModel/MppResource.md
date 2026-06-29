# MppResource

A resource (a person, piece of equipment, or material that work is assigned to). Value type;
copyable and equality-comparable.

```cpp
#include "src/model/mppresource.h"
```

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `uniqueId` | `int` | Stable identity. Referenced by assignments (`resourceUniqueId`) and by resource calendars. |
| `id` | `int` | Display / sheet position (the resource's ID column). |
| `name` | `QString` | Resource name. |
| `initials` | `QString` | Resource initials. |
| `maxUnits` | `double` | Maximum units available, as a ratio. `1.0` == 100%. |
| `notes` | `QString` | The resource's notes as raw RTF source (empty if none). |
| `cost` | `double` | Total cost, in the project's currency unit (rolled up from assignments when not stored). |
| `actualCost` | `double` | Cost incurred so far. |
| `remainingCost` | `double` | Cost still to be incurred. |
| `costVariance` | `double` | Cost minus baseline cost. |
| `baselines` | `QList<MppBaseline>` | Saved baselines (cost and work only; see [`MppBaseline`](MppBaseline.md)). |
| `customFields` | `QList<MppCustomField>` | Populated custom/extended fields (see [`MppCustomField`](MppCustomField.md)). |
| `costRates` | `QList<MppCostRate>` | Cost-rate tables A–E with time-phased rates (see [`MppCostRate`](MppCostRate.md)). |

## Notes

* Resources are linked to tasks through [`MppAssignment`](MppAssignment.md), not directly.
* A resource may also have a *resource calendar* — an [`MppCalendar`](MppCalendar.md) whose name is
  the resource's name and whose `baseCalendarUniqueId` points at a base calendar.
* Resource **cost rates** (standard rate, overtime rate, cost-per-use) live in Microsoft Project's
  cost-rate tables and are decoded into `costRates` (see [`MppCostRate`](MppCostRate.md)).
* **`cost`** is the resource's stored cost where present; for resources whose cost Microsoft Project
  computes from rate tables, MppIO rolls it up from the resource's assignment costs so it matches the
  exported value.

## Example

```cpp
for (const MppResource &r : project.resources)
    qInfo() << r.id << r.name << "(" << r.initials << ")"
            << "max" << int(r.maxUnits * 100) << "%";
```
