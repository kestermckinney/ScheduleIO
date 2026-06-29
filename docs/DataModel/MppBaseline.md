# MppBaseline

One saved baseline snapshot of a task, resource, or assignment. Value type; copyable and
equality-comparable.

```cpp
#include "src/model/mppbaseline.h"
```

Microsoft Project stores a *current* baseline (number `0`) plus up to ten saved baselines
(numbers `1`–`10`). Each [`MppTask`](MppTask.md), [`MppResource`](MppResource.md) and
[`MppAssignment`](MppAssignment.md) carries a `QList<MppBaseline>` holding only the baselines that
are actually saved in the file.

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `number` | `int` | Baseline number: `0` = current baseline, `1`–`10` = saved baselines. |
| `cost` | `double` | Baseline cost, in the project's currency unit. |
| `workMillis` | `qint64` | Baseline work, in milliseconds. |
| `start` | `QDateTime` | Baseline start (UTC). Invalid if not set. |
| `finish` | `QDateTime` | Baseline finish (UTC). Invalid if not set. |
| `durationMillis` | `qint64` | Baseline duration, in milliseconds. |

## Notes

* Which fields are populated depends on the entity: tasks carry all of them; **resources** store
  only `cost` and `workMillis`; **assignments** store `cost`, `workMillis`, `start` and `finish`.
  Unset fields keep their defaults (`0` or an invalid `QDateTime`).
* A baseline appears in the list only if the file has at least one of its fields saved.

## Example

```cpp
for (const MppTask &t : project.tasks) {
    for (const MppBaseline &b : t.baselines) {
        qInfo().noquote() << t.name << "baseline" << b.number
                          << "cost" << b.cost
                          << "start" << b.start.toString(Qt::ISODate)
                          << "work(h)" << b.workMillis / 3600000.0;
    }
}
```
