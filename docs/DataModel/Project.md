# schedule::Project

`schedule::Project` is the top-level schedule document and owner of every entity collection. A
successful `MppIO::open()` or `XmlIO::open()` fills this value; access it with `project()` or provide
an edited value with `setProject()`.

```cpp
#include "model/project.h"
```

## Identity, metadata, and schedule bounds

| Member | Type | Default | Meaning |
| :--- | :--- | :--- | :--- |
| `formatVersion` | `FormatVersion` | `Unknown` | Detected/desired binary format family. |
| `title` | `QString` | empty | Document title. |
| `author` | `QString` | empty | Document author. |
| `startDate` | `QDateTime` | invalid | Project start boundary. |
| `finishDate` | `QDateTime` | invalid | Project finish boundary. |
| `scheduleFromStart` | `bool` | `true` | Forward schedule from `startDate`; `false` schedules backward from `finishDate`. |
| `multipleCriticalPaths` | `bool` | `false` | Anchor independent networks at their own finish when computing critical paths. |
| `statusDate` | `QDateTime` | invalid | As-of date for progress and earned-value calculations. |
| `calendarUniqueId` | `int` | `-1` | Project calendar UID; `-1` selects the calendar named Standard/built-in Standard. |
| `budgetCost` | `double` | `0.0` | Project-level budget cost. |
| `budgetWorkMillis` | `qint64` | `0` | Project-level budget work in milliseconds. |

`FormatVersion` values are `Unknown = 0`, `Mpp12 = 12` (Project 2007), and `Mpp14 = 14`
(Project 2010 and later). For binary output, `Unknown` defaults to native MPP14; explicit MPP12
output uses the legacy ScheduleIO scaffold.

## Owned collections

| Member | Type | Meaning |
| :--- | :--- | :--- |
| `tasks` | `QList<Task>` | Task rows, including a project summary row when present. |
| `resources` | `QList<Resource>` | Work, material, and cost resources. |
| `assignments` | `QList<Assignment>` | Links between task and resource UIDs, with work/cost detail. |
| `calendars` | `QList<Calendar>` | Base and derived working-time calendars. |
| `relations` | `QList<Relation>` | Predecessor/successor dependency links. |
| `customFieldDefinitions` | `QList<CustomField>` | Formula, lookup, and indicator metadata. Entity lists carry the values. |

## View and presentation state

| Member | Type | Meaning |
| :--- | :--- | :--- |
| `viewStyles` | `ViewStyles` | Gantt Chart text, grid/date lines, and default bar styles. |
| `resourceUsageStyles` | `ViewStyles` | Resource Usage style set; may fall back to `viewStyles`. |
| `teamPlannerStyles` | `ViewStyles` | Team Planner style set; may fall back to `viewStyles`. |
| `calendarStyles` | `ViewStyles` | Calendar view style set; may fall back to `viewStyles`. |
| `ganttView` | `UsageViewSettings` | Native Gantt table columns and pane geometry. |
| `resourceUsageView` | `UsageViewSettings` | Resource Usage table, details, timescale, and pane geometry. |
| `taskUsageView` | `UsageViewSettings` | Task Usage table, detail selection, timescale, and pane geometry. |
| `teamPlannerView` | `UsageViewSettings` | Team Planner table and geometry state. |
| `timelineView` | `TimelineViewSettings` | Timeline bars, members, text styles, and options. |
| `reportAccentColor` | `qint32` | Report color (`0xRRGGBB`) or `TextStyle::kAutomatic`; native MPP persistence is limited. |

See [View and Timeline Formatting](Formatting.md) for the nested structures and writer flags.

## Schedule defaults

| Member | Default | Meaning |
| :--- | :--- | :--- |
| `newTasksManual` | `false` | New tasks are manually scheduled when true. |
| `newTaskStartIsProjectStart` | `true` | Start new tasks at project start; false selects current-date behavior. |
| `defaultTaskType` | `0` | `0` Fixed Units, `1` Fixed Duration, `2` Fixed Work. |
| `defaultDurationUnits` | `7` | Default `Duration::Unit`; `7` is Days. |
| `defaultWorkUnits` | `2` | MSPDI work display code; `2` is hours. |
| `newTasksEffortDriven` | `false` | New tasks use effort-driven scheduling. |
| `autoLinkTasks` | `true` | Automatically link moved/inserted tasks in compatible clients. |
| `splitInProgressTasks` | `true` | Progress updating may split incomplete work. |
| `honorConstraints` | `false` | Resource leveling honors task constraints when true. |
| `criticalSlackLimit` | `0` | Whole-day slack threshold used to classify critical tasks. |

## Calendar defaults

| Member | Default | Meaning |
| :--- | :--- | :--- |
| `weekStartDay` | `0` | `0` Sunday through `6` Saturday (MSPDI convention, not Qt's weekday enum). |
| `fiscalYearStartMonth` | `1` | Fiscal-year start month, `1` through `12`. |
| `fiscalYearUsesStartYear` | `false` | Label the fiscal year by its starting year when true. |
| `defaultStartTime` | `08:00` | Default task start time. |
| `defaultEndTime` | `17:00` | Default task finish time. |
| `minutesPerDay` | `480` | Working minutes represented by one non-elapsed day. |
| `minutesPerWeek` | `2400` | Working minutes represented by one non-elapsed week. |
| `daysPerMonth` | `20` | Working days represented by one non-elapsed month. |

The persisted profile is per project. `Duration::setWorkingTimeProfile()` controls the process-global
conversion profile used by duration helpers while an application has a project active.

## Calculation options

| Member | Default | Meaning |
| :--- | :--- | :--- |
| `moveCompletedEndsBack` | `false` | Move completed portions ending after the status date back to it. |
| `moveRemainingStartsBack` | `false` | Move remaining portions starting after the status date back to it. |
| `moveRemainingStartsForward` | `false` | Move remaining portions starting before the status date forward to it. |
| `moveCompletedEndsForward` | `false` | Move completed portions ending before the status date forward to it. |
| `statusUpdatesResource` | `true` | Updating task status also updates resource/assignment status. |

Use `ProgressUpdating` to perform progress operations; changing an option alone does not recalculate
the project.

## Financial and display options

| Member | Default | Meaning |
| :--- | :--- | :--- |
| `currencySymbol` | empty | Project currency symbol; empty leaves the client default. |
| `currencySymbolPosition` | `0` | `0` before, `1` after, `2` before with space, `3` after with space. |
| `currencyDigits` | `2` | Decimal digits displayed for currency. |
| `currencyCode` | empty | Currency code such as `USD`. |
| `defaultStandardRate` | `0.0` | Standard rate used for new resources. |
| `defaultOvertimeRate` | `0.0` | Overtime rate used for new resources. |
| `defaultFixedCostAccrual` | `3` | `1` Start, `2` End, `3` Prorated. |
| `defaultEarnedValueMethod` | `0` | `0` percent complete, `1` physical percent complete. |
| `baselineForEarnedValue` | `0` | Baseline slot used for earned value, `0` through `10`. |
| `showProjectSummaryTask` | `true` | Show the project summary task. |

## Opaque MPP preservation buffers

| Member | Purpose |
| :--- | :--- |
| `mppFontBases` | Exact MPP14 font-table payload needed to preserve native font indices. |
| `mppBarExceptions` | Per-task native Gantt bar formatting bytes beyond modeled `Task::barColor`. |
| `mppSourceTemplate` | Original MPP14 container used to preserve native presentation/view streams during an edited save. |

These buffers are intentionally excluded from semantic equality. Preserve them when editing a
project loaded from MPP if native presentation fidelity matters. They are implementation aids, not
portable schedule content.

## Format coverage and equality

Project options currently round-trip through MSPDI; native MPP support depends on the individual
decoded property. View settings also have format-specific boundaries. Consult
[Field Coverage](../Reference/FieldCoverage.md) rather than assuming every public member is writable
to every format.

`Project::operator==` compares modeled scalar state and child collections but intentionally omits
opaque source buffers and selected presentation bookkeeping. It tests semantic model equality, not
file-byte identity.

## Example

```cpp
const schedule::Project &project = io.project();
qInfo() << project.title
        << project.tasks.size() << "tasks"
        << project.resources.size() << "resources"
        << project.assignments.size() << "assignments";
```
