# MppAssignment

Links a resource to a task. Value type; copyable and equality-comparable.

```cpp
#include "src/model/mppassignment.h"
```

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `uniqueId` | `int` | The assignment's own unique id. |
| `taskUniqueId` | `int` | Unique id of the assigned [`MppTask`](MppTask.md). |
| `resourceUniqueId` | `int` | Unique id of the assigned [`MppResource`](MppResource.md). |
| `units` | `double` | Assigned units, as a ratio. `1.0` == 100%. |
| `workMillis` | `qint64` | Assigned work, in milliseconds. |
| `notes` | `QString` | The assignment's notes as raw RTF source (empty if none). |
| `cost` | `double` | Total cost, in the project's currency unit. |
| `actualCost` | `double` | Cost incurred so far. |
| `remainingCost` | `double` | Cost still to be incurred. |
| `costVariance` | `double` | Cost minus baseline cost. |
| `baselines` | `QList<MppBaseline>` | Saved baselines (cost, work, start, finish; see [`MppBaseline`](MppBaseline.md)). |
| `customFields` | `QList<MppCustomField>` | Populated custom/extended fields (see [`MppCustomField`](MppCustomField.md)). |

## Resolving the links

An assignment references its task and resource by unique id. Join them with lookups:

```cpp
QHash<int, const MppTask*>     taskById;
QHash<int, const MppResource*> resById;
for (const MppTask &t : project.tasks)     taskById.insert(t.uniqueId, &t);
for (const MppResource &r : project.resources) resById.insert(r.uniqueId, &r);

for (const MppAssignment &a : project.assignments) {
    const MppTask     *t = taskById.value(a.taskUniqueId);
    const MppResource *r = resById.value(a.resourceUniqueId);
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
