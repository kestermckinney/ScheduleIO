# MppProject

The top-level document. A successful `MppIO::open()` fills one of these; reach it with
[`MppIO::project()`](../API/MppIO.md#const-mppproject-project-const).

```cpp
#include "src/model/mppproject.h"
```

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `formatVersion` | `MppProject::FormatVersion` | The detected binary format family (see below). |
| `title` | `QString` | Project title (from the document summary information). |
| `author` | `QString` | Project author. |
| `startDate` | `QDateTime` | Project start date (UTC). |
| `finishDate` | `QDateTime` | Project finish date (UTC). |
| `tasks` | `QList<MppTask>` | All tasks. See [MppTask](MppTask.md). |
| `resources` | `QList<MppResource>` | All resources. See [MppResource](MppResource.md). |
| `assignments` | `QList<MppAssignment>` | Task ↔ resource assignments. See [MppAssignment](MppAssignment.md). |
| `relations` | `QList<MppRelation>` | Predecessor links. See [MppRelation](MppRelation.md). |
| `calendars` | `QList<MppCalendar>` | Calendars. See [MppCalendar](MppCalendar.md). |

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

`MppProject` defines `operator==` / `operator!=`. Two projects are equal when all scalar fields and
all child collections are element-wise equal. This makes it straightforward to compare a model before
and after a transformation.

## Example

```cpp
const MppProject &p = io.project();
qInfo() << p.title << "by" << p.author
        << "—" << p.tasks.size() << "tasks,"
        << p.resources.size() << "resources";
```
