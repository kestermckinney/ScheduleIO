# XML Interchange

Alongside the binary-`.mpp` reader ([`MppIO`](../API/MppIO.md)), the library provides
[`XmlIO`](../API/XmlIO.md) for **Microsoft Project compatible XML** — the MSPDI format Microsoft
Project exports and imports as `.xml`. Both classes populate the same
[`schedule::Project`](../DataModel/Project.md), so they compose directly.

```cpp
#include "xmlio.h"
```

## Reading an XML file

```cpp
#include "xmlio.h"
#include <QDebug>

XmlIO io;
if (!io.open("Schedule.xml")) {
    qWarning() << "Failed to read XML:" << io.errorString();
    return;
}

const schedule::Project &project = io.project();
qInfo() << "Title:" << project.title;
qInfo() << "Tasks:" << project.tasks.size();
```

The resulting `schedule::Project` is identical in shape to one produced by `MppIO`, so everything in
[Basic Usage](BasicUsage.md) — iterating tasks, joining assignments, reading baselines, custom
fields, and notes — applies unchanged.

## Writing an XML file

Set a project, then `save()`. The output is a standard MSPDI document that Microsoft Project can
open:

```cpp
schedule::Project project = buildProject();   // or one you read earlier

XmlIO io;
io.setProject(project);
if (!io.save("Schedule.xml"))
    qWarning() << "Failed to write XML:" << io.errorString();
```

To keep the bytes in memory instead of writing a file, use `saveToData()`:

```cpp
XmlIO io;
io.setProject(project);
const QByteArray xml = io.saveToData();   // UTF-8 MSPDI document
```

## Converting `.mpp` → `.xml`

Because both facades share the model, converting a binary file to XML that Microsoft Project can
open is just *read with one, write with the other*:

```cpp
#include "mppio.h"
#include "xmlio.h"

MppIO in;
if (!in.open("Schedule.mpp")) {
    qWarning() << in.errorString();
    return;
}

XmlIO out;
out.setProject(in.project());          // hand the model straight over
if (!out.save("Schedule.xml"))
    qWarning() << out.errorString();
```

## Converting `.xml` → a model

The reverse — load an XML interchange file and work with it (or hand it to your own code) — is just
as direct:

```cpp
XmlIO in;
if (in.open("Schedule.xml"))
    analyse(in.project());
```

!!! note "XML and binary output"
    `XmlIO` produces Microsoft Project compatible **XML**. For a native binary Project 2010+ file,
    pass the same model to `MppIO` and save it as MPP14.

## Round-tripping

`XmlIO` is a lossless reader/writer for the schedule-data fields it maps: reading a document, writing
it, and reading it again preserves those fields. This is verified against real Microsoft Project
exports in `tst_xml_roundtrip`; `tst_xml_mpp_crosscheck` also loads the same project from MPP and XML
and compares their shared schedule data. MPP view/row/cell formatting and font-base payloads are not
represented in MSPDI output.

## Loading at run time

`XmlIO` ships in the same shared library as `MppIO` and exports a matching `extern "C"` factory:

```cpp
extern "C" {
    XmlIO      *xmlio_create();
    void        xmlio_destroy(XmlIO *io);
    const char *xmlio_version();
}
```

Resolve and use it exactly as shown for `MppIO` in [Dynamic Loading](DynamicLoading.md), substituting
the `xmlio_*` names.
