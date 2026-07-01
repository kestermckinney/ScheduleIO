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
| `percentComplete` | `double` | Completion ratio, `0.0`–`1.0`. |
| `milestone` | `bool` | `true` if the task is a milestone. |
| `summary` | `bool` | `true` if the task has child tasks (or is the project summary). |
| `constraintType` | `int` | Scheduling constraint code (`0` = *As Soon As Possible*). |
| `constraintDate` | `QDateTime` | Constraint date, when the constraint type requires one; otherwise invalid. |
| `wbs` | `QString` | Work Breakdown Structure code (the dotted outline number, e.g. `1.2.1`). |
| `notes` | `QString` | The task's notes as raw RTF source (empty if none). |
| `cost` | `double` | Total cost, in the project's currency unit. |
| `fixedCost` | `double` | Fixed cost (cost not derived from resource work). |
| `actualCost` | `double` | Cost incurred so far. |
| `remainingCost` | `double` | Cost still to be incurred. |
| `costVariance` | `double` | Cost minus baseline cost. |
| `baselines` | `QList<schedule::Baseline>` | Saved baselines (see [`schedule::Baseline`](Baseline.md)); empty if none saved. |
| `customFields` | `QList<schedule::CustomField>` | Populated custom/extended fields (see [`schedule::CustomField`](CustomField.md)). |

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

## Example

```cpp
for (const schedule::Task &t : project.tasks) {
    const QString indent(t.outlineLevel * 2, ' ');
    qInfo().noquote() << t.wbs << indent + t.name
                      << int(t.percentComplete * 100) << "%";
}
```
