# schedule::Task

A single task (a row in the Gantt chart). Value type; copyable and equality-comparable.

```cpp
#include "src/model/task.h"
```

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `uniqueId` | `int` | Stable identity across edits (the task's Unique ID). Referenced by assignments and relations. |
| `id` | `int` | Display / outline position (the task's ID column). |
| `outlineLevel` | `int` | Outline depth. `0` is the project-summary row; top-level tasks are `1`. |
| `name` | `QString` | Task name. |
| `start` | `QDateTime` | Scheduled start (UTC). Invalid if unset. |
| `finish` | `QDateTime` | Scheduled finish (UTC). Invalid if unset. |
| `durationMillis` | `qint64` | Duration in milliseconds. |
| `durationFormat` | `int` | Microsoft Project display-unit code (`schedule::Duration::Unit`). |
| `workMillis` | `qint64` | Task-level work in milliseconds. |
| `percentComplete` | `double` | Completion ratio, `0.0`–`1.0`. |
| `milestone` | `bool` | `true` if the task is a milestone. |
| `summary` | `bool` | `true` if the task has child tasks (or is the project summary). |
| `constraintType` | `int` | Scheduling constraint code (`0` = *As Soon As Possible*). |
| `constraintDate` | `QDateTime` | Constraint date, when the constraint type requires one; otherwise invalid. |
| `wbs` | `QString` | Work Breakdown Structure code (the dotted outline number, e.g. `1.2.1`). |
| `notes` | `QString` | The task's notes as raw RTF source (empty if none). |
| `active` | `bool` | `false` for an inactive task excluded from scheduling and rollups. |
| `manual` | `bool` | Whether the task is manually scheduled. |
| `levelingDelayMillis` | `qint64` | Working-time delay added by resource leveling. |
| `effortDriven` | `bool` | Keep total work stable when resources are added or removed. |
| `taskType` | `int` | `0` fixed units, `1` fixed duration, `2` fixed work. |
| `priority` | `int` | Leveling priority from 0 to 1000. |
| `deadline` | `QDateTime` | Deadline, or invalid when unset. |
| `calendarUniqueId` | `int` | Task calendar UID, or `-1` to use the project calendar. |
| `lateStart`, `lateFinish` | `QDateTime` | Backward-pass dates computed by `Scheduler::computeSlack()`. |
| `totalSlackMillis`, `freeSlackMillis` | `qint64` | Computed slack in working milliseconds. |
| `critical` | `bool` | Whether total slack is zero or negative. |
| `actualStart`, `actualFinish` | `QDateTime` | Recorded actual dates. |
| `actualDurationMillis`, `actualWorkMillis` | `qint64` | Recorded actual duration and work. |
| `evm` | `schedule::EarnedValue` | Stored PV, EV, AC, CV, SV, CPI, SPI, EAC, and TCPI values. |
| `cost` | `double` | Total cost, in the project's currency unit. |
| `fixedCost` | `double` | Fixed cost (cost not derived from resource work). |
| `actualCost` | `double` | Cost incurred so far. |
| `remainingCost` | `double` | Cost still to be incurred. |
| `costVariance` | `double` | Cost minus baseline cost. |
| `baselines` | `QList<schedule::Baseline>` | Saved baselines (see [`schedule::Baseline`](Baseline.md)); empty if none saved. |
| `customFields` | `QList<schedule::CustomField>` | Populated custom/extended fields (see [`schedule::CustomField`](CustomField.md)). |
| `rowFormat` | `schedule::TextStyle` | Per-row font, emphasis, foreground/background, and pattern. |
| `barColor` | `qint32` | Per-task bar color or `TextStyle::kAutomatic` (binary MPP persistence is limited). |
| `cellFormats` | `QHash<QString, schedule::TextStyle>` | Per-column text/background overrides. |

## Notes

* **Manual vs. automatic scheduling.** `start` and `finish` are the *effective* scheduled dates and
  are correct for both automatically- and manually-scheduled tasks.
* **Summary and WBS are derived.** Microsoft Project computes a task's summary flag and WBS from the
  outline hierarchy rather than storing them; MppIO derives them the same way, in `id` order.
* **Constraint type** mirrors Microsoft Project's constraint enumeration (0 ASAP, 1 ALAP, 2 Must
  Start On, 3 Must Finish On, 4 Start No Earlier Than, 5 Start No Later Than, 6 Finish No Earlier
  Than, 7 Finish No Later Than).
* **Cost** values are plain currency amounts (not cents) in the project's currency. Summary-task
  costs are Microsoft Project's rolled-up totals.
* **Baselines** are only present when the file has saved them. `baselines` may hold any subset of
  the eleven baseline slots (number `0` is the current baseline, `1`–`10` are saved baselines).
* **`notes`** is the *raw RTF* source exactly as Microsoft Project stores it (e.g.
  `{\rtf1\ansi ...}`). MppIO does not strip it to plain text; use an RTF parser if you need that.
* **Scheduling results are explicit fields.** Call `Scheduler::reschedule()` after dependency/date
  edits and `Scheduler::computeSlack()` when late dates and critical-path values must be refreshed.
* **Formatting colors** are `0xRRGGBB`; `TextStyle::kAutomatic` means inherit the view default.

## Example

```cpp
for (const schedule::Task &t : project.tasks) {
    const QString indent(t.outlineLevel * 2, ' ');
    qInfo().noquote() << t.wbs << indent + t.name
                      << int(t.percentComplete * 100) << "%";
}
```
