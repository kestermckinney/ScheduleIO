# Basic Usage

The entry point is the [`MppIO`](../API/MppIO.md) class. You open a file, then read the populated
[`MppProject`](../DataModel/MppProject.md) it produces.

## Opening a file

```cpp
#include "mppio.h"
#include <QDebug>

MppIO io;
if (!io.open("Schedule.mpp")) {
    qWarning() << "Failed to read .mpp:" << io.errorString();
    return;
}

const MppProject &project = io.project();
qInfo() << "Title:"  << project.title;
qInfo() << "Author:" << project.author;
qInfo() << "Tasks:"  << project.tasks.size();
```

`open()` returns `false` on any problem (file missing, not a valid `.mpp` container, unsupported
format version). When it returns `false`, [`errorString()`](../API/MppIO.md#errorstring) describes
why.

## Iterating tasks

Every collection on `MppProject` is a `QList` of value types, so ordinary range-for works:

```cpp
for (const MppTask &task : project.tasks) {
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
QHash<int, const MppResource*> byId;
for (const MppResource &r : project.resources)
    byId.insert(r.uniqueId, &r);

for (const MppAssignment &a : project.assignments) {
    const MppResource *res = byId.value(a.resourceUniqueId);
    qInfo() << "Task" << a.taskUniqueId
            << "<-" << (res ? res->name : QStringLiteral("?"))
            << "units" << a.units;
}
```

## Predecessor links

Schedule dependencies are in `project.relations`. Each [`MppRelation`](../DataModel/MppRelation.md)
names a predecessor and a successor task by unique id, plus the link type:

```cpp
for (const MppRelation &link : project.relations) {
    qInfo() << "Task" << link.successorTaskUid
            << "depends on task" << link.predecessorTaskUid
            << "type" << link.type;
}
```

## Cost, baselines, and custom fields

Each task carries cost values, any saved baselines, and the populated custom ("extended") fields:

```cpp
for (const MppTask &task : project.tasks) {
    qInfo() << task.name << "cost" << task.cost
            << "fixed" << task.fixedCost
            << "variance" << task.costVariance;

    // Saved baselines (number 0 = current baseline, 1..10 = saved baselines).
    for (const MppBaseline &b : task.baselines)
        qInfo() << "   baseline" << b.number
                << "cost" << b.cost
                << "start" << b.start.toString(Qt::ISODate);

    // Only the custom slots that are actually set are present.
    for (const MppCustomField &c : task.customFields)
        qInfo() << "   " << c.name << "=" << c.value;
}
```

Resources and assignments expose the same `cost`, `baselines` and `customFields` members. See
[`MppBaseline`](../DataModel/MppBaseline.md) and [`MppCustomField`](../DataModel/MppCustomField.md)
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

## A note on saving

`MppIO::save()` / `saveToData()` serialise the model to the library's **own** internal format, not
the Microsoft binary `.mpp` layout. They exist so the model can be round-tripped (read → write →
read) without loss. MppIO does **not** write Microsoft Project binary files.

To produce a file Microsoft Project can open, save the same model as **Microsoft Project compatible
XML** with [`XmlIO`](../API/XmlIO.md) — it shares this object model, so it is a drop-in for output:

```cpp
#include "xmlio.h"

XmlIO xml;
xml.setProject(io.project());   // the model you just read from the .mpp
xml.save("Schedule.xml");       // standard MSPDI; opens in Microsoft Project
```

See [XML Interchange](XmlInterchange.md) for the full read/write API and `.mpp` ↔ `.xml` conversion.
