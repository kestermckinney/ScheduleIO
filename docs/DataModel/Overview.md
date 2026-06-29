# Data Model Overview

When you read a file, MppIO fills in a single [`MppProject`](MppProject.md) object. Everything else
hangs off it as `QList`s of value types.

```
MppProject
├── title, author, startDate, finishDate, formatVersion
├── tasks        : QList<MppTask>
├── resources    : QList<MppResource>
├── assignments  : QList<MppAssignment>
├── relations    : QList<MppRelation>
└── calendars    : QList<MppCalendar>
```

## Design principles

**Plain value types.** Every model class — `MppProject`, `MppTask`, `MppResource`,
`MppAssignment`, `MppRelation`, `MppCalendar` — is a copyable struct-like type with public data
members and an `operator==`. There are no getters/setters, no ownership semantics, and no Qt object
parent/child relationships. You can copy them freely, store them in containers, and compare whole
models for equality.

**Linked by unique id.** The collections are flat lists, not a nested tree. Cross-references use
integer **unique ids**:

| From | Field | Refers to |
| :--- | :--- | :--- |
| `MppAssignment` | `taskUniqueId` | an `MppTask.uniqueId` |
| `MppAssignment` | `resourceUniqueId` | an `MppResource.uniqueId` |
| `MppRelation` | `predecessorTaskUid`, `successorTaskUid` | `MppTask.uniqueId` |
| `MppCalendar` | `baseCalendarUniqueId` | another `MppCalendar.uniqueId` |

Build a `QHash<int, …>` keyed on `uniqueId` when you need fast lookups (see
[Basic Usage](../GettingStarted/BasicUsage.md)).

**The outline hierarchy** is expressed through each task's `outlineLevel` and `wbs`, in task `id`
order — there is no parent pointer. A task is a `summary` when the following task (by `id`) is one
outline level deeper.

## Common Qt types used

| Type | Meaning |
| :--- | :--- |
| `QString` | text fields (names, WBS, title, author) |
| `QDateTime` | dates and times, in **UTC**; invalid means "no date" |
| `qint64` | durations and work, in **milliseconds** |
| `double` | ratios such as assignment units and resource max units (`1.0` == 100%) |
| `int` | unique ids, display ids, outline levels, enum-like codes |

## Per-type reference

* [MppProject](MppProject.md)
* [MppTask](MppTask.md)
* [MppResource](MppResource.md)
* [MppAssignment](MppAssignment.md)
* [MppRelation](MppRelation.md)
* [MppCalendar](MppCalendar.md)
