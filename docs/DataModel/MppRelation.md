# MppRelation

A task dependency (predecessor link). The *successor* task depends on the *predecessor* task. Value
type; copyable and equality-comparable.

```cpp
#include "src/model/mpprelation.h"
```

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `uniqueId` | `int` | The relation's own unique id. |
| `predecessorTaskUid` | `int` | Unique id of the predecessor [`MppTask`](MppTask.md). |
| `successorTaskUid` | `int` | Unique id of the successor task. |
| `type` | `int` | Link type — see `MppRelation::Type` below. |
| `lagMillis` | `qint64` | Lag/lead between the tasks, in milliseconds. |

## Type

```cpp
enum Type {
    FinishToStart  = 1,   // most common: successor starts after predecessor finishes
    StartToStart   = 3,
    FinishToFinish = 0,
    StartToFinish  = 2,
};
```

`type` holds one of these codes (matching Microsoft Project's link-type numbering).

## Notes

* **`lagMillis`** is positive for a *lag* (delay) and negative for a *lead* (overlap). Like other
  durations it is the working-time amount normalised to milliseconds (e.g. a `5d` lag on an 8h/day
  calendar is 40 hours).

## Example

```cpp
for (const MppRelation &link : project.relations) {
    const char *kind = link.type == MppRelation::FinishToStart ? "FS"
                     : link.type == MppRelation::StartToStart  ? "SS"
                     : link.type == MppRelation::FinishToFinish ? "FF" : "SF";
    qInfo() << "task" << link.successorTaskUid
            << "after task" << link.predecessorTaskUid << kind;
}
```
