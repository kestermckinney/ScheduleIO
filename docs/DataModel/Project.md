# schedule::Project

The top-level document. A successful `MppIO::open()` fills one of these; reach it with
[`MppIO::project()`](../API/MppIO.md#const-scheduleproject-project-const).

```cpp
#include "src/model/project.h"
```

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `formatVersion` | `schedule::Project::FormatVersion` | The detected binary format family (see below). |
| `title` | `QString` | Project title (from the document summary information). |
| `author` | `QString` | Project author. |
| `startDate` | `QDateTime` | Project start date (UTC). |
| `finishDate` | `QDateTime` | Project finish date (UTC). |
| `scheduleFromStart` | `bool` | `true` schedules forward from `startDate`; `false` schedules backward from `finishDate`. |
| `statusDate` | `QDateTime` | As-of date for progress and earned-value calculations. |
| `calendarUniqueId` | `int` | Project calendar UID; `-1` selects the calendar named `Standard`. |
| `tasks` | `QList<schedule::Task>` | All tasks. See [schedule::Task](Task.md). |
| `resources` | `QList<schedule::Resource>` | All resources. See [schedule::Resource](Resource.md). |
| `assignments` | `QList<schedule::Assignment>` | Task ↔ resource assignments. See [schedule::Assignment](Assignment.md). |
| `relations` | `QList<schedule::Relation>` | Predecessor links. See [schedule::Relation](Relation.md). |
| `calendars` | `QList<schedule::Calendar>` | Calendars. See [schedule::Calendar](Calendar.md). |
| `viewStyles` | `schedule::ViewStyles` | Gantt text, line, and standard bar styles. |
| `resourceUsageStyles` | `schedule::ViewStyles` | Resource Usage view styles. |
| `teamPlannerStyles` | `schedule::ViewStyles` | Team Planner view styles. |
| `calendarStyles` | `schedule::ViewStyles` | Calendar view styles. |
| `reportAccentColor` | `qint32` | Report primary-series color (`0xRRGGBB`) or `TextStyle::kAutomatic`; persistence is currently limited to the internal scaffold. |
| `mppFontBases` | `QByteArray` | Opaque MPP14 font table retained for exact binary font-index preservation. |

## FormatVersion

```cpp
enum class FormatVersion {
    Unknown = 0,
    Mpp12   = 12,   // Microsoft Project 2007
    Mpp14   = 14,   // Microsoft Project 2010 and later
};
```

`formatVersion` reports which `.mpp` family the file belongs to, detected from the file's version
marker. Most modern files are `Mpp14`. For output, `Unknown` defaults to native MPP14; explicit
`Mpp12` output uses the legacy internal scaffold.

## Equality

`schedule::Project` defines `operator==` / `operator!=` for semantic model comparison. Scalar fields,
styles, and child collections are compared element by element. Opaque storage aids such as the exact
`mppFontBases` payload and a `TextStyle`'s raw font-table index are deliberately excluded when their
friendly font/style values are equivalent.

## Example

```cpp
const schedule::Project &p = io.project();
qInfo() << p.title << "by" << p.author
        << "—" << p.tasks.size() << "tasks,"
        << p.resources.size() << "resources";
```
