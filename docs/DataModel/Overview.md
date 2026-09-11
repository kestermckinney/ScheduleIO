# Data Model Overview

Every successful `MppIO::open()` or `XmlIO::open()` produces one `schedule::Project`. The project
owns all model data by value; there are no `QObject` parents, owning pointers, or hidden live file
handles behind its child collections.

```text
schedule::Project
├── tasks : QList<Task>
│   ├── TaskSegment, EarnedValue, Baseline, CustomField
│   └── TextStyle row/cell formatting
├── resources : QList<Resource>
│   └── AvailabilityPeriod, Baseline, CostRate, CustomField
├── assignments : QList<Assignment>
│   └── Baseline, CustomField, TimephasedValue
├── relations : QList<Relation>
├── calendars : QList<Calendar>
│   └── TimeRange, CalendarException
├── customFieldDefinitions : QList<CustomField>
└── ViewStyles, UsageViewSettings, TimelineViewSettings
```

## Value semantics and ownership

The model classes are struct-like, copyable Qt value types with public members. Most define
`operator==`, making snapshots and semantic round-trip tests straightforward:

```cpp
schedule::Project before = io.project();
schedule::Project edited = before;
edited.title = QStringLiteral("Revised plan");
io.setProject(edited);
```

`MppIO::project()` and `XmlIO::project()` return a reference owned by the I/O object. That reference
remains valid only until the object is destroyed, opens another document, or receives
`setProject()`. Copy the project when it must outlive or be edited independently of the facade.

## Identity and links

Entity collections are flat. Stable integer unique IDs form the links:

| Source | Member | Target |
| :--- | :--- | :--- |
| `Project` | `calendarUniqueId` | `Calendar::uniqueId` |
| `Task` | `calendarUniqueId` | `Calendar::uniqueId` |
| `Resource` | `calendarUniqueId` | usually a derived `Calendar::uniqueId` |
| `Assignment` | `taskUniqueId` | `Task::uniqueId` |
| `Assignment` | `resourceUniqueId` | `Resource::uniqueId` |
| `Relation` | `predecessorTaskUid` | predecessor `Task::uniqueId` |
| `Relation` | `successorTaskUid` | successor `Task::uniqueId` |
| `Calendar` | `baseCalendarUniqueId` | base `Calendar::uniqueId` |
| `TimelineItem` | `taskUid` | `Task::uniqueId` |
| `TimelineItem` | `barId` | `TimelineBar::id` |

An entity's `id` is its current sheet/display position; its `uniqueId` is the stable key. Do not use
display IDs to join collections. A `QHash<int, const T *>` is convenient for repeated lookups, but
remember that pointers into a Qt container may be invalidated when that container is modified.

Task hierarchy is also flat. `outlineLevel`, `wbs`, and task `id` order describe the outline; there
is no parent pointer. Summary rows roll up their following descendants.

## Units and conventions

| Representation | Meaning |
| :--- | :--- |
| `QDateTime` | Date and time. File codecs normalize schedule timestamps to UTC; an invalid value means not set. |
| `QDate` / `QTime` | Calendar-only date or time-of-day values. |
| `qint64` ending in `Millis` | Duration or work in milliseconds. Whether time is working or elapsed depends on its field/unit. |
| `double` units/percent fields | Ratio: `1.0` means 100%, `0.5` means 50%. |
| `double` cost/rate fields | Ordinary currency amount, not integer cents. Currency metadata lives on `Project`. |
| `qint32` colors | `0xRRGGBB`; `TextStyle::kAutomatic` (`-1`) means inherit/theme automatic. |
| `int` unit/type fields | Microsoft Project-compatible numeric codes documented on the owning type. |
| raw notes | RTF source, not plain text. Empty means no notes. |

Common sentinels are `-1` for an absent UID/reference, an invalid Qt date/time for no date, `0` for
an unset numeric value, and an empty collection when the source carries no rows. Read each member's
reference because zero can also be a valid Microsoft Project enum value.

## Persisted, derived, transient, and opaque values

The model contains four kinds of state:

* **Persisted semantic fields** are decoded to named members and written when supported by the chosen
  output format.
* **Derived fields** include WBS/summary information, rollups, slack, and some costs. Scheduling or
  reconciliation utilities refresh them after related edits.
* **Transient bookkeeping** such as `Task::levelingAnchor` and `modified` flags controls an edit or
  writer pass and is not a document field.
* **Opaque preservation payloads** such as `Project::mppSourceTemplate`, `mppFontBases`,
  `mppBarExceptions`, and `TimelineViewSettings::rawXml` preserve native data that is only partly
  modeled. Do not clear them during an edit unless discarding source presentation state is intended.

The same structure does not imply identical MPP and MSPDI coverage. Consult
[Field Coverage](../Reference/FieldCoverage.md) before relying on a read/edit/write cycle.

## Equality

Equality is semantic rather than a byte-for-byte comparison of an input file. Opaque preservation
buffers, writer bookkeeping, and selected native font-table details are intentionally excluded where
they do not represent a modeled schedule value. `Project::operator==` is useful for model round trips;
it does not prove binary identity.

## Editing and recalculation

Changing a public member changes only that member. Use the matching utility when dependent state must
be updated:

| Edit | Follow-up |
| :--- | :--- |
| Work, duration, assignment units/resources | `TaskScheduling` |
| Dependencies, constraints, start/finish anchors | `Scheduler::reschedule()` |
| Late dates, slack, critical flags | `Scheduler::computeSlack()` |
| Time-phased work/cost, aggregate rollups | `ProjectReconciliation::reconcile()` |
| Resource conflicts | `ResourceLeveling::level()` |
| Status-date progress | `ProgressUpdating` |
| Material quantities/rates | `MaterialCosting::recalculate()` |
| Predefined work contour | `WorkContouring::apply()` |

See [Editing and Scheduling](../GettingStarted/EditingAndScheduling.md) and the
[Scheduling API](../API/Scheduling.md) for the intended operation order.

## Complete type reference

* Core entities: [Project](Project.md), [Task](Task.md), [Resource](Resource.md),
  [Assignment](Assignment.md), [Relation](Relation.md), [Calendar](Calendar.md)
* Supporting values: [Baseline](Baseline.md), [AvailabilityPeriod](Availability.md),
  [CostRate](CostRate.md), [CustomField](CustomField.md),
  [TimephasedValue](TimephasedValue.md)
* Shared conventions: [Duration and Units](Duration.md)
* Presentation state: [View and Timeline Formatting](Formatting.md)
* Calculation return/configuration types: [Scheduling Helper Types](SchedulingTypes.md)
