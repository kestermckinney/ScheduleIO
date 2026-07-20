# Basic Usage

The entry point is the [`MppIO`](../API/MppIO.md) class. You open a file, then read the populated
[`schedule::Project`](../DataModel/Project.md) it produces.

## Opening a file

```cpp
#include "mppio.h"
#include <QDebug>

MppIO io;
if (!io.open("Schedule.mpp")) {
    qWarning() << "Failed to read .mpp:" << io.errorString();
    return;
}

const schedule::Project &project = io.project();
qInfo() << "Title:"  << project.title;
qInfo() << "Author:" << project.author;
qInfo() << "Tasks:"  << project.tasks.size();
```

`open()` returns `false` on any problem (file missing, not a valid `.mpp` container, unsupported
format version). When it returns `false`, [`errorString()`](../API/MppIO.md#errorstring) describes
why.

## Iterating tasks

Every collection on `schedule::Project` is a `QList` of value types, so ordinary range-for works:

```cpp
for (const schedule::Task &task : project.tasks) {
    qInfo().noquote()
        << task.wbs.leftJustified(8)
        << task.name
        << "start" << task.start.toString(Qt::ISODate)
        << "finish" << task.finish.toString(Qt::ISODate);

    if (task.milestone)
        qInfo() << "   (milestone)";
}
```

All dates are `QDateTime` values in **UTC** (the wall-clock time matches Microsoft Project's display).
A "no date" field is returned as an **invalid** `QDateTime` (`QDateTime::isValid()` is `false`).

## Durations

Durations and work are stored in milliseconds. Convert to whatever unit you need:

```cpp
const double hours = task.durationMillis / (1000.0 * 60.0 * 60.0);
qInfo() << task.name << "duration" << hours << "hours";
```

## Resources and assignments

Tasks, resources, and assignments are linked by **unique id**. Build a lookup if you need to join
them:

```cpp
QHash<int, const schedule::Resource*> byId;
for (const schedule::Resource &r : project.resources)
    byId.insert(r.uniqueId, &r);

for (const schedule::Assignment &a : project.assignments) {
    const schedule::Resource *res = byId.value(a.resourceUniqueId);
    qInfo() << "Task" << a.taskUniqueId
            << "<-" << (res ? res->name : QStringLiteral("?"))
            << "units" << a.units;
}
```

## Predecessor links

Schedule dependencies are in `project.relations`. Each [`schedule::Relation`](../DataModel/Relation.md)
names a predecessor and a successor task by unique id, plus the link type:

```cpp
for (const schedule::Relation &link : project.relations) {
    qInfo() << "Task" << link.successorTaskUid
            << "depends on task" << link.predecessorTaskUid
            << "type" << link.type;
}
```

## Cost, baselines, and custom fields

Each task carries cost values, any saved baselines, and the populated custom ("extended") fields:

```cpp
for (const schedule::Task &task : project.tasks) {
    qInfo() << task.name << "cost" << task.cost
            << "fixed" << task.fixedCost
            << "variance" << task.costVariance;

    // Saved baselines (number 0 = current baseline, 1..10 = saved baselines).
    for (const schedule::Baseline &b : task.baselines)
        qInfo() << "   baseline" << b.number
                << "cost" << b.cost
                << "start" << b.start.toString(Qt::ISODate);

    // Only the custom slots that are actually set are present.
    for (const schedule::CustomField &c : task.customFields)
        qInfo() << "   " << c.name << "=" << c.value;
}
```

Resources and assignments expose the same `cost`, `baselines` and `customFields` members. See
[`schedule::Baseline`](../DataModel/Baseline.md) and [`schedule::CustomField`](../DataModel/CustomField.md)
for the field details and value types.

Tasks, resources, and assignments also carry a `notes` field. It holds the **raw RTF source** of
the notes exactly as Microsoft Project stores it — MppIO does not strip it to plain text:

```cpp
if (!task.notes.isEmpty())
    qInfo() << task.name << "notes (RTF):" << task.notes;
```

## Reading from memory

If you already have the file bytes (for example from a download), use `openFromData()`:

```cpp
QByteArray bytes = downloadMpp();
MppIO io;
if (io.openFromData(bytes))
    process(io.project());
```

## Editing and saving

Copy the value model, edit it, put it back into the facade, and save. MPP14 is a native binary output
format; an `Unknown` format version defaults to MPP14.

```cpp
schedule::Project edited = io.project();
edited.title = "Updated schedule";
edited.tasks[0].name = "Confirm requirements";

io.setProject(edited);
if (!io.save("Updated-schedule.mpp"))
    qWarning() << io.errorString();
```

MPP12 files can be read, but selecting `FormatVersion::Mpp12` for output uses the legacy internal
scaffold rather than a Microsoft-compatible MPP12 writer. Use MPP14 for native binary delivery.

You can also save the same model as **Microsoft Project compatible XML** with
[`XmlIO`](../API/XmlIO.md):

```cpp
#include "xmlio.h"

XmlIO xml;
xml.setProject(io.project());   // the model you just read from the .mpp
xml.save("Schedule.xml");       // standard MSPDI; opens in Microsoft Project
```

See [Editing and Scheduling](EditingAndScheduling.md) for construction, dependency scheduling,
resource leveling, formatting, and output examples. See [XML Interchange](XmlInterchange.md) for the
full XML API and `.mpp` ↔ `.xml` conversion.
