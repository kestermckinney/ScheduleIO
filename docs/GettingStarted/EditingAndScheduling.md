# Editing, Scheduling, and Saving

ScheduleIO's model is made of public value types. A normal editing workflow is: read or construct a
`schedule::Project`, modify a copy, run the scheduling helpers that apply to the edit, then save with
`MppIO` (binary MPP14) or `XmlIO` (MSPDI XML).

## Build a small project

```cpp
#include "mppio.h"
#include "src/model/calendar.h"
#include "src/model/duration.h"
#include "src/model/scheduler.h"

schedule::Project project;
project.formatVersion = schedule::Project::FormatVersion::Mpp14;
project.title = "Website launch";
project.startDate = QDateTime(QDate(2026, 8, 3), QTime(8, 0), Qt::UTC);
project.calendars = schedule::Calendar::microsoftDefaults();
project.calendarUniqueId = 1; // Standard

schedule::Task design;
design.uniqueId = 1;
design.id = 1;
design.name = "Design";
design.start = project.startDate;
design.durationMillis = schedule::Duration::toMillis(3, schedule::Duration::Days);
design.durationFormat = schedule::Duration::Days;
design.finish = schedule::Scheduler::projectCalendar(project)
                    .addWork(design.start, design.durationMillis);

schedule::Task build = design;
build.uniqueId = 2;
build.id = 2;
build.name = "Build";
build.durationMillis = schedule::Duration::toMillis(5, schedule::Duration::Days);

project.tasks = { design, build };

schedule::Relation dependency;
dependency.uniqueId = 1;
dependency.predecessorTaskUid = design.uniqueId;
dependency.successorTaskUid = build.uniqueId;
dependency.type = schedule::Relation::FinishToStart;
project.relations.append(dependency);

schedule::Scheduler::reschedule(project);
schedule::Scheduler::computeSlack(project);

MppIO out;
out.setProject(project);
if (!out.save("Website-launch.mpp"))
    qWarning() << out.errorString();
```

Use stable, non-zero unique IDs for tasks, resources, assignments, relations, and calendars. Display
IDs are separate: a task's `id` controls row order, while `uniqueId` is what links use.

## Parse and display durations

The model stores duration, work, and lag canonically in milliseconds while retaining a display-unit
code.

```cpp
qint64 duration = 0;
int unit = schedule::Duration::Days;
if (schedule::Duration::parse("2.5 days", &duration, &unit)) {
    task.durationMillis = duration;
    task.durationFormat = unit;
    qInfo() << schedule::Duration::format(duration, unit); // "2.5 days"
}
```

Elapsed units use wall-clock time. Ordinary days/weeks/months use Microsoft Project defaults of 8
hours/day, 40 hours/week, and 20 days/month.

## Change work, duration, or assignment units

Use `TaskScheduling` for edits that participate in the Work = Duration x Units scheduling triangle.
It applies fixed-units, fixed-duration, fixed-work, and effort-driven behavior consistently.

```cpp
#include "src/model/taskscheduling.h"

const int assignmentUid =
    schedule::TaskScheduling::addAssignment(project, taskUid, resourceUid, 1.0);

schedule::TaskScheduling::setAssignmentUnits(project, assignmentUid, 0.5);
schedule::TaskScheduling::setDuration(
    project, taskUid,
    schedule::Duration::toMillis(4, schedule::Duration::Days));

// Push successors after the task's finish changes.
schedule::Scheduler::reschedule(project);
schedule::Scheduler::computeSlack(project);
```

## Prevent dependency cycles

Before adding predecessor P to successor T, reject the edit if P is already reachable from T:

```cpp
if (schedule::Scheduler::reachable(project, successorUid, predecessorUid)) {
    qWarning() << "That link would create a dependency cycle";
} else {
    project.relations.append(newLink);
    schedule::Scheduler::reschedule(project);
}
```

## Inspect and level resource load

```cpp
#include "src/model/resourceleveling.h"

const auto conflicts = schedule::ResourceLeveling::overallocations(project);
for (const auto &c : conflicts)
    qInfo() << c.resourceUniqueId << c.start << c.finish
            << c.peakUnits << ">" << c.maxUnits;

const int delayedTasks = schedule::ResourceLeveling::level(project);
qInfo() << delayedTasks << "tasks delayed";

// To undo later:
schedule::ResourceLeveling::clearLeveling(project);
```

Leveling does not move manually scheduled or already-started tasks. It records working-time delay on
the task and assignment, rescheduling successors as it proceeds.

## Apply row and cell formatting

Colors use `0xRRGGBB`; `TextStyle::kAutomatic` means inherit Microsoft Project's automatic style.

```cpp
schedule::Task &task = project.tasks[0];
task.rowFormat.bold = true;
task.rowFormat.color = 0x1F4E78;
task.rowFormat.backColor = 0xD9EAF7;
task.rowFormat.backPattern = 1; // solid
task.rowFormat.fontName = "Aptos";
task.rowFormat.fontSize = 11;

schedule::TextStyle durationCell;
durationCell.italic = true;
durationCell.backColor = 0xFFF2CC;
durationCell.backPattern = 1;
task.cellFormats.insert("5", durationCell); // ScheduleVault's stable Duration-column key
```

Custom-field column keys use `c:<field name>`. Row and per-cell formatting is stored in the MPP14
view property streams. Category styles, gridlines, date lines, and standard bar colors are available
through `Project::viewStyles` and the other view-specific style members.

## Choose an output format

```cpp
// Binary Project 2010+ file.
MppIO binary;
binary.setProject(project);
binary.save("Plan.mpp");

// Human-readable Microsoft Project XML interchange.
XmlIO xml;
xml.setProject(project);
xml.save("Plan.xml");
```

Use MPP14 for native binary delivery. Use XML when another tool expects MSPDI or when a transparent,
diffable interchange file is more useful. Check `errorString()` after any failed operation.
