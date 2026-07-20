# Data Model Overview

When you read a file, MppIO fills in a single [`schedule::Project`](Project.md) object. Everything else
hangs off it as `QList`s of value types.

```
schedule::Project
├── metadata, dates, default calendar, format version
├── tasks        : QList<schedule::Task>
├── resources    : QList<schedule::Resource>
├── assignments  : QList<schedule::Assignment>
├── relations    : QList<schedule::Relation>
├── calendars    : QList<schedule::Calendar>
└── view styles  : Gantt, Resource Usage, Team Planner, Calendar
```

## Design principles

**Plain value types.** Every model class — `schedule::Project`, `schedule::Task`, `schedule::Resource`,
`schedule::Assignment`, `schedule::Relation`, `schedule::Calendar` — is a copyable struct-like type with public data
members and an `operator==`. There are no getters/setters, no ownership semantics, and no Qt object
parent/child relationships. You can copy them freely, store them in containers, and compare whole
models for equality.

**Linked by unique id.** The collections are flat lists, not a nested tree. Cross-references use
integer **unique ids**:

| From | Field | Refers to |
| :--- | :--- | :--- |
| `schedule::Assignment` | `taskUniqueId` | a `schedule::Task.uniqueId` |
| `schedule::Assignment` | `resourceUniqueId` | a `schedule::Resource.uniqueId` |
| `schedule::Relation` | `predecessorTaskUid`, `successorTaskUid` | `schedule::Task.uniqueId` |
| `schedule::Calendar` | `baseCalendarUniqueId` | another `schedule::Calendar.uniqueId` |
| `schedule::Project` | `calendarUniqueId` | the project calendar |
| `schedule::Task` | `calendarUniqueId` | an optional task calendar |
| `schedule::Resource` | `calendarUniqueId` | an optional resource calendar |

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

## Editing the model

The I/O classes do not retain pointers into these lists. Copy a project, edit its public fields, and
pass the value back with `setProject()`. Use `TaskScheduling`, `Scheduler`, `ResourceLeveling`, and
`WorkCalendar` when an edit needs Microsoft Project-like recalculation. See
[Editing and Scheduling](../GettingStarted/EditingAndScheduling.md).

## Per-type reference

* [schedule::Project](Project.md)
* [schedule::Task](Task.md)
* [schedule::Resource](Resource.md)
* [schedule::Assignment](Assignment.md)
* [schedule::Relation](Relation.md)
* [schedule::Calendar](Calendar.md)
