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
| `tasks` | `QList<schedule::Task>` | All tasks. See [schedule::Task](Task.md). |
| `resources` | `QList<schedule::Resource>` | All resources. See [schedule::Resource](Resource.md). |
| `assignments` | `QList<schedule::Assignment>` | Task ↔ resource assignments. See [schedule::Assignment](Assignment.md). |
| `relations` | `QList<schedule::Relation>` | Predecessor links. See [schedule::Relation](Relation.md). |
| `calendars` | `QList<schedule::Calendar>` | Calendars. See [schedule::Calendar](Calendar.md). |

## FormatVersion

```cpp
enum class FormatVersion {
    Unknown = 0,
    Mpp12   = 12,   // Microsoft Project 2007
    Mpp14   = 14,   // Microsoft Project 2010 and later
};
```

`formatVersion` reports which `.mpp` family the file belongs to, detected from the file's version
marker. Most modern files are `Mpp14`.

## Equality

`schedule::Project` defines `operator==` / `operator!=`. Two projects are equal when all scalar fields and
all child collections are element-wise equal. This makes it straightforward to compare a model before
and after a transformation.

## Example

```cpp
const schedule::Project &p = io.project();
qInfo() << p.title << "by" << p.author
        << "—" << p.tasks.size() << "tasks,"
        << p.resources.size() << "resources";
```
