# schedule::CostRate

One time-phased entry of a resource cost-rate table. Value type; copyable and equality-comparable.

```cpp
#include "src/model/costrate.h"
```

Microsoft Project gives each resource up to five cost-rate tables (**A–E**, `table` 0–4). Each table
holds a list of entries; an entry's rates apply over a date range, so a resource whose rate changes
over time has several entries in the same table. [`schedule::Resource::costRates`](Resource.md) is the
flat list of all such entries across all tables for that resource.

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `table` | `int` | Which cost-rate table this entry belongs to: `0`–`4` == A–E. |
| `startDate` | `QDateTime` | Effective from (invalid == open start). |
| `endDate` | `QDateTime` | Effective to (invalid == open / "until further notice"). |
| `standardRate` | `double` | Standard rate, expressed per `standardRateUnit`. |
| `standardRateUnit` | `int` | Rate time unit, Microsoft's format code (`2` = per hour, `3` = per day, `4` = per week, …). |
| `overtimeRate` | `double` | Overtime rate, expressed per `overtimeRateUnit`. |
| `overtimeRateUnit` | `int` | Overtime rate time unit (same coding as `standardRateUnit`). |
| `costPerUse` | `double` | Per-use cost, in the project's currency unit. |

## Notes

* Entries within a table are ordered by `endDate`; the entry with an **invalid `endDate`** is the
  current (open-ended) rate. Use that one for "the resource's current rate".
* Rates are stored per-hour in the file and converted to the displayed unit using Microsoft's default
  minutes-per-day/-week (480 / 2400). For the common per-hour rates this is an exact value.
* Microsoft Project often does not store table A explicitly when a resource has no rate; such a
  resource simply has no entries for that table.

## Example

```cpp
for (const schedule::Resource &r : project.resources) {
    for (const schedule::CostRate &cr : r.costRates) {
        if (cr.table == 0 && !cr.endDate.isValid())   // table A, current rate
            qInfo() << r.name << "rate" << cr.standardRate
                    << "per-unit" << cr.standardRateUnit;
    }
}
```
