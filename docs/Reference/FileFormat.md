# MPP File Format and Storage

This page explains the binary `.mpp` representation that ScheduleIO reads and writes, and how the
library owns the file bytes and in-memory model. It is useful when debugging a file, adding a field,
or deciding what a save operation will preserve.

!!! warning "A reverse-engineered format"
    Microsoft does not publish the application-level MPP record layout. The OLE Compound File
    container is documented as MS-CFB, but the Project-specific storages, streams, field maps, and
    records described here were established from real files and cross-checked against Microsoft
    Project XML exports. `DECODING_NOTES.md` contains the lower-level research log.

## The four layers

An MPP file is not one flat byte sequence. ScheduleIO processes four layers:

| Layer | Representation | ScheduleIO implementation |
| :--- | :--- | :--- |
| Compound document | OLE2/MS-CFB directory, storages, streams, FAT and mini-FAT | `src/ole/compoundfile.*` |
| Project containers | `Props14`, `   114`, `   214`, summary-information streams | `src/serializer/*`, `src/codec/propsreader.*` |
| Entity records | fixed records, variable blobs, metadata, field maps | `src/codec/*` |
| Public model | `schedule::Project` and its child value types | `src/model/*` |

The read path moves down this table. The write path starts with the public model and builds the
container in the opposite direction.

## OLE compound-file container

An OLE compound file behaves like a small filesystem inside one file:

* A **storage** is a directory.
* A **stream** is a named byte array.
* The root directory contains both streams and child storages.

ScheduleIO supports version-3 compound files: 512-byte regular sectors, 64-byte mini sectors, and a
4,096-byte mini-stream cutoff. Streams smaller than 4,096 bytes are packed into the mini stream;
larger streams use regular FAT chains. The reader walks the DIFAT, FAT, mini-FAT, and directory tree,
checking offsets and guarding every chain against cycles. Invalid structures cause `MppIO::open()`
to return `false` instead of exposing partial data.

The internal `CompoundFile` object materializes the complete directory tree and every stream as
`QByteArray` data. It does not memory-map the source file and it does not retain a `QFile` handle.
Consequently, after `open()` returns, the source file may be moved or closed without affecting the
loaded project.

### Important root entries

```text
Root Entry
|-- Props14                         MPP14 version marker
|-- \x01CompObj                      OLE component metadata
|-- \x05SummaryInformation           title and author property set
|-- \x05DocumentSummaryInformation   other document metadata
|--    114/                         main project data (three leading spaces)
|   |-- Props                       project properties and field maps
|   |-- TBkndTask/
|   |-- TBkndRsc/
|   |-- TBkndAssn/
|   |-- TBkndCons/
|   `-- TBkndCal/
`--    214/                         views, tables, filters, reports and formatting
    |-- Props                       includes the font-base table
    `-- CV_iew/                     view metadata and property blobs
```

The leading spaces in `   114` and `   214` are part of the names. Do not trim them when inspecting
or constructing paths.

## Format detection

`DocSerializer::detectVersion()` looks for a root `Props14` or `Props12` stream. The result is stored
in `Project::formatVersion`:

| Value | Meaning | Read | Write |
| :--- | :--- | :---: | :---: |
| `Mpp12` | Project 2007 family | yes | internal scaffold only |
| `Mpp14` | Project 2010 and later family | yes | real template-based MPP14 |
| `Unknown` | no recognized marker | rejected on read; defaults to MPP14 for a new model | n/a |

Set `formatVersion` to `Mpp14` when constructing a project. Leaving it `Unknown` is also valid:
`MppIO::saveToData()` chooses MPP14 automatically.

## Main data storage (`   114`)

The main storage contains one Bknd sub-storage for each modeled entity:

| Storage | Model output | Record identity |
| :--- | :--- | :--- |
| `TBkndTask` | `Project::tasks` | task unique ID |
| `TBkndRsc` | `Project::resources` | resource unique ID |
| `TBkndAssn` | `Project::assignments` | assignment unique ID |
| `TBkndCons` | `Project::relations` | predecessor-link unique ID |
| `TBkndCal` | `Project::calendars` | calendar unique ID |

Tasks, resources, and assignments are linked by unique ID, not by list index. The task display order
is carried by task ID/outline fields and is independent of the order of fixed records.

### The six entity streams

A normal MPP14 entity storage has two fixed-record pairs and one variable-data pair:

| Stream | Purpose |
| :--- | :--- |
| `FixedMeta` | header plus one metadata entry per primary fixed record; entries point into `FixedData` and contain presence/deletion flags |
| `FixedData` | concatenated primary fixed-width records |
| `Fixed2Meta` | metadata for the secondary fixed blocks |
| `Fixed2Data` | secondary records, including fields such as manual-scheduling dates |
| `VarMeta` | directory from `(unique ID, field type)` to an offset in `Var2Data` |
| `Var2Data` | length-prefixed variable blobs such as UTF-16 names, notes, calendars, baselines, and custom data |

Both metadata formats begin with the magic value `0xFADFADBA`. A `VarMeta` header is 24 bytes,
followed by 12-byte entries:

```text
[u32 uniqueId][u32 offset][u16 fieldTypeLow][u16 fieldTypeHigh]
```

At `offset`, a typical `Var2Data` value starts with a 32-bit byte length followed by its payload.
Strings are UTF-16LE. Not every variable blob is a string; calendar, baseline, rate, and formatting
payloads have their own codecs.

`FixedMeta` and `Fixed2Meta` divide their matching data stream into records. The metadata entry size
depends on the entity and format. ScheduleIO reads the record offsets rather than assuming that
records are densely packed, and honors deletion/presence flags so stale rows do not enter the model.

## Props and field maps

`   114/Props` is a typed key/value container. Its 16-byte header is followed by items shaped as:

```text
[u32 dataLength][u32 key][u32 flags][dataLength bytes of payload]
```

The low 16 bits of a key identify a property and the high bits encode its storage type. Project-wide
values such as start, finish, status date, title, and default-calendar name live here.

The same stream also contains field maps for tasks, resources, and assignments. A field map relates
a Microsoft Project field ID to a fixed block and byte offset, or to a variable-data type. This is
why ScheduleIO does not hard-code one monolithic C++ record layout: the reader resolves locations
through the file's map, and the MPP14 writer uses the embedded template's authentic map when placing
values.

Examples of project property keys used by the library include:

| Key | Value |
| :--- | :--- |
| `0x02400002` | project start date |
| `0x02400003` | project finish date |
| `0x02400008` | project title |
| `0x02400045` | status date |
| `0x0240000E` | default calendar name |

The standard `\x05SummaryInformation` property set is also read and regenerated for title/author
metadata. The project title is patched in both the Project props and summary information because
Microsoft Project consults the Project-specific value when opening the schedule.

## View and formatting storage (`   214`)

The view storage contains tables, filters, reports, and view definitions. ScheduleIO currently maps:

* Gantt, Resource Usage, Team Planner, and Calendar view text/category styles;
* Task Usage and Resource Usage native table-column widths and timescale size;
* gridline and date-line styles;
* standard Gantt bar colors;
* task row formatting and per-column cell formatting;
* font family, point size, emphasis, foreground/background color, and background pattern.

The font table is the `FONT_BASES` payload at `   214/Props` key `0x03400000`. Formatting records
refer to it by index. `Project::mppFontBases` retains the opaque source payload so a read/write can
preserve an exact index mapping; friendly style values are exposed through `TextStyle`.

Per-row/per-column formatting records store a value and a separate **change mask**. A false value is
not always the same as an inherited value. In particular, ScheduleIO emits the bold-change bit on a
whole-row record even when `rowFormat.bold` is `false`; that explicit normal-weight override prevents
Microsoft Project from inheriting a bold category style for the formatted row.

## Read pipeline

`MppIO::open()` performs these steps:

1. Read all file bytes into memory.
2. Parse and validate the compound-file container.
3. Detect MPP12 or MPP14 from the version marker.
4. Parse project props and entity field maps.
5. Decode each entity's fixed and variable streams into value objects.
6. Resolve derived information such as outline/WBS, calendar links, and view formatting.
7. Replace the `MppIO` instance's project only after the complete parse succeeds.

That last rule is useful for recovery: a failed `open()` reports an error and does not install a
half-decoded replacement model.

```cpp
MppIO io;
if (!io.open("Plan.mpp")) {
    qWarning() << io.errorString();
    return;
}

// This reference is owned by io and remains valid until io is destroyed,
// setProject() is called, or a later successful open replaces the model.
const schedule::Project &project = io.project();
```

## MPP14 write pipeline

For MPP14, ScheduleIO emits a real OLE/Microsoft Project binary container. The writer:

1. Opens the embedded empty-project template in `src/serializer/mpp14template.mpp`.
2. Copies the template storage tree, including version markers and unmodified view/table/filter
   infrastructure.
3. Regenerates the six streams in `TBkndTask`, `TBkndRsc`, `TBkndAssn`, `TBkndCons`, and
   `TBkndCal` from the public model.
4. Uses the template field maps and record geometry to place each modeled field.
5. Patches project props, summary information, font bases, and supported view formatting.
6. Rebuilds a deterministic version-3 compound file, choosing mini or regular sectors by size.

The source MPP container is not edited in place. A save is built from a stock template plus the
current model, so unknown source-only records are not automatically preserved. Fields called out as
unsupported in [Field Coverage](FieldCoverage.md) should be treated as potentially lost on resave.

Resource calendars deserve one special note: MPP represents a resource's calendar using a derived
per-resource calendar row. The writer materializes those rows on a private copy of the model; it does
not mutate the caller's `Project`.

### Save an edited MPP14 file

```cpp
MppIO io;
if (!io.open("Plan.mpp"))
    return;

schedule::Project edited = io.project();
edited.title = "Release plan";
edited.tasks[0].name = "Confirm scope";

io.setProject(edited);       // copies the value model into io
if (!io.save("Plan-updated.mpp"))
    qWarning() << io.errorString();
```

### Save and reopen entirely in memory

```cpp
MppIO writer;
writer.setProject(project);
const QByteArray image = writer.saveToData();
if (image.isEmpty())
    qFatal("%s", qPrintable(writer.errorString()));

MppIO check;
Q_ASSERT(check.openFromData(image));
Q_ASSERT(check.project() == project);
```

## Ownership and memory behavior

The public API deliberately uses values rather than pointers into the file:

| Operation | Ownership behavior |
| :--- | :--- |
| `open(path)` | reads the entire file; the path and `QFile` are not retained |
| `openFromData(bytes)` | parses the supplied bytes; no reference to the caller's array is retained |
| `project()` | returns a const reference owned by that `MppIO` instance |
| `setProject(project)` | copies the complete value model into the instance |
| `saveToData()` | returns a new `QByteArray` containing the complete compound file |
| `save(path)` | builds the complete byte image, then writes it to the destination |

For long-lived or concurrent work, copy the project out and give each operation its own `MppIO`
instance. `MppIO` is non-copyable and does not promise thread-safe mutation of one shared instance;
independent instances can be used independently.

## Current boundaries

* Reading supports real MPP12 and MPP14 families; writing a Microsoft-compatible binary file is
  supported for MPP14.
* MPP12 saving uses ScheduleIO's legacy scaffold container for internal round-trip compatibility.
  Do not select MPP12 when the destination must open in Microsoft Project.
* Only version-3 OLE compound files (512-byte sectors) are accepted.
* A save preserves the modeled project and supported formatting, not arbitrary unmodeled streams
  from the original source container.
* XML remains a useful transparent interchange/debugging format through `XmlIO`, but is no longer
  the only Microsoft Project-compatible output option.
