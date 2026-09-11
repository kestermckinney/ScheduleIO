# schedule::AvailabilityPeriod

One dated row in a work resource's **Resource Availability** table. `Resource::availabilityTable`
stores these rows as copyable, equality-comparable values.

```cpp
#include "model/availability.h"
```

## Members

| Member | Type | Default | Meaning |
| :--- | :--- | :--- | :--- |
| `startDate` | `QDateTime` | invalid | First instant the row applies; invalid means an open start (`NA` in Microsoft Project). |
| `endDate` | `QDateTime` | invalid | Last boundary the row applies to; invalid means an open end. |
| `units` | `double` | `1.0` | Maximum available units for the period; `1.0` is 100%. |

Rows describe capacity, not assignment demand. `Resource::maxUnits` is the general capacity;
availability rows override it over their effective ranges. Resource leveling uses that dated capacity
when detecting conflicts. Material and cost resources do not use labor availability.

Keep rows ordered by effective date and avoid overlapping ranges when constructing a project. An
empty table means the resource's `maxUnits` applies throughout the schedule.

```cpp
schedule::AvailabilityPeriod summer;
summer.startDate = QDateTime(QDate(2026, 6, 1), QTime(0, 0), Qt::UTC);
summer.endDate = QDateTime(QDate(2026, 8, 31), QTime(23, 59), Qt::UTC);
summer.units = 0.5; // half time
resource.availabilityTable.append(summer);
```
