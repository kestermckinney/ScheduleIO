# Scheduling Utilities

ScheduleIO separates file I/O from recalculation. Editing a public field changes that value only;
call the appropriate utility when related dates, work, slack, or resource load must be recomputed.
All utilities operate on `schedule::Project` values and do not perform file I/O.

## Which utility to call

| Change or question | Utility |
| :--- | :--- |
| Parse/display a duration or lag | `schedule::Duration` |
| Walk working time and calendar exceptions | `schedule::WorkCalendar` |
| Recalculate Work = Duration x Units | `schedule::TaskScheduling` |
| Move tasks from dependency/constraint changes | `schedule::Scheduler::reschedule()` |
| Calculate late dates, slack, and critical flags | `schedule::Scheduler::computeSlack()` |
| Find or remove resource conflicts | `schedule::ResourceLeveling` |

## Duration

Include `src/model/duration.h`. `Duration::parse()` accepts values such as `3d`, `2.5 wks`, `4
hours`, and `2 edays`. `toMillis()`, `fromMillis()`, and `format()` convert between the canonical
millisecond value and Microsoft Project's display units.

```cpp
qint64 value;
int unit;
if (schedule::Duration::parse("1.5 weeks", &value, &unit))
    qInfo() << value << schedule::Duration::format(value, unit);
```

## WorkCalendar

Include `src/model/workcalendar.h`. Construct with a project and calendar UID to flatten its base
chain, weekly hours, and exceptions. An unknown UID uses the built-in Standard calendar.

```cpp
schedule::WorkCalendar calendar(project, task.calendarUniqueId);
task.start = calendar.nextWorkStart(task.start);
task.finish = calendar.addWork(task.start, task.durationMillis);
const qint64 spanWork = calendar.workBetween(task.start, task.finish);
```

Useful methods are `isWorkingDay()`, `workingTimes()`, `nextWorkStart()`, `prevWorkEnd()`,
`addWork()`, `workBetween()`, and `workPerDay()`.

## TaskScheduling

Include `src/model/taskscheduling.h`. These operations maintain the scheduling triangle and update
assignment spans for leaf tasks:

```cpp
schedule::TaskScheduling::setDuration(project, taskUid, durationMillis);
schedule::TaskScheduling::setWork(project, taskUid, workMillis);
schedule::TaskScheduling::setAssignmentUnits(project, assignmentUid, 0.75);
schedule::TaskScheduling::setAssignmentWork(project, assignmentUid, workMillis);
const int uid = schedule::TaskScheduling::addAssignment(project, taskUid, resourceUid, 1.0);
schedule::TaskScheduling::removeAssignment(project, uid);
schedule::TaskScheduling::syncTask(project, taskUid);
```

The implementation honors fixed units, fixed duration, fixed work, and effort-driven assignment
changes. Resource calendars are not yet consulted by this scheduling triangle; it uses the task's
calendar or the project calendar.

## Scheduler

Include `src/model/scheduler.h`.

`reschedule()` performs the dependency-driven forward pass for auto-scheduled leaf tasks. It handles
FS, SS, FF, and SF links, positive lag/negative lead, leveling delay, and the supported date
constraints. Manual and summary tasks are not moved. No-predecessor tasks keep their current start
as an anchor.

`computeSlack()` performs the backward pass, filling `lateStart`, `lateFinish`, total/free slack, and
`critical`. Call it after `reschedule()`.

```cpp
schedule::Scheduler::reschedule(project);
schedule::Scheduler::computeSlack(project);
```

`reachable(project, fromUid, toUid)` follows predecessor-to-successor links and is intended for
cycle prevention before inserting a relation.

## ResourceLeveling

Include `src/model/resourceleveling.h`.

`overallocations()` returns time windows in which concurrent assignment units exceed resource
capacity. `level()` delays eligible tasks until conflicts are gone and returns the number delayed.
The default uses Priority/Standard order. An `Options` value can instead select ID Only or Standard
order, restrict movement to available total slack, and restrict which tasks may receive delay.
`clearLeveling()` removes those delays and reschedules.

Other helpers answer whether one resource is overallocated, calculate a resource's total work, and
spread an assignment's work over a requested time period.

```cpp
for (const auto &window : schedule::ResourceLeveling::overallocations(project))
    qInfo() << window.resourceUniqueId << window.start << window.finish;

schedule::ResourceLeveling::level(project);

schedule::ResourceLeveling::Options options;
options.order = schedule::ResourceLeveling::Order::Standard;
options.levelOnlyWithinAvailableSlack = true;
options.taskUniqueIds = { 12, 14 }; // empty means all tasks
schedule::ResourceLeveling::level(project, options);
// ...
schedule::ResourceLeveling::clearLeveling(project);
```

## ProgressUpdating

Include `src/model/progressupdating.h`. `rescheduleIncompleteWork()` implements the
desktop `Update Project` / `pjReschedule` subset. It preserves actual dates and
time-phased actual work, moves remaining assignment buckets after the supplied
boundary using the applicable task/resource calendars, and recalculates successors.
An empty task set updates the whole project; a non-empty set limits the operation.
Summary, manual, inactive, and completed tasks are protected.

```cpp
project.statusDate = QDateTime(QDate(2026, 8, 5), QTime(17, 0));
const int moved = schedule::ProgressUpdating::rescheduleIncompleteWork(
    project, project.statusDate, { 12, 14 });
```

## Recommended edit sequence

For a change that affects schedule logic:

1. Modify the model directly or through `TaskScheduling`.
2. Reject a new relation if `Scheduler::reachable()` shows it would form a cycle.
3. Call `Scheduler::reschedule()`.
4. Optionally call `ResourceLeveling::level()`.
5. Call `Scheduler::computeSlack()`.
6. Put the value into `MppIO` or `XmlIO` and save.
