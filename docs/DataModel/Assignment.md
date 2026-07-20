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
| `units` | `double` | Assigned units, as a ratio. `1.0` == 100%. |
| `workMillis` | `qint64` | Assigned work, in milliseconds. |
| `start`, `finish` | `QDateTime` | Scheduled assignment span; invalid means use the task span. |
| `delayMillis` | `qint64` | Assignment delay in working milliseconds. |
| `levelingDelayMillis` | `qint64` | Delay added by resource leveling. |
| `actualWorkMillis` | `qint64` | Work completed. |
| `remainingWorkMillis` | `qint64` | Work still scheduled. |
| `notes` | `QString` | The assignment's notes as raw RTF source (empty if none). |
| `cost` | `double` | Total cost, in the project's currency unit. |
| `actualCost` | `double` | Cost incurred so far. |
| `remainingCost` | `double` | Cost still to be incurred. |
| `costVariance` | `double` | Cost minus baseline cost. |
| `baselines` | `QList<schedule::Baseline>` | Saved baselines (cost, work, start, finish; see [`schedule::Baseline`](Baseline.md)). |
| `customFields` | `QList<schedule::CustomField>` | Populated custom/extended fields (see [`schedule::CustomField`](CustomField.md)). |

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
