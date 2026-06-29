# File Format Notes

This page is a high-level orientation to how a `.mpp` file is structured and how MppIO is organised
internally. It is background for contributors; you do not need any of it to *use* the library — see
[Basic Usage](../GettingStarted/BasicUsage.md) for that.

The `.mpp` format is undocumented. MppIO's understanding was reverse-engineered from real files and
cross-checked against Microsoft Project XML exports; the detailed, evolving notes live in
`DECODING_NOTES.md` in the repository.

## Layers

A `.mpp` file is an **OLE2 compound document** (the same container used by legacy `.doc`/`.xls`).
MppIO peels it apart in layers:

| Layer | Source file | Role |
| :--- | :--- | :--- |
| Compound file | `src/ole/compoundfile.*` | Reads the OLE2 / [MS-CFB] container — the tree of *storages* and *streams*. |
| Props container | `src/codec/propsreader.*` | Parses the `Props` streams (key → value blobs), including the field maps. |
| Var data | `src/codec/bkndvardata.*` | Decodes an entity's `VarMeta` + `Var2Data` pair (variable-length fields such as names). |
| Field codecs | `src/codec/fielddecoders.*` | Low-level scalar decoders: timestamps, durations, strings, GUIDs. |
| Serializers | `src/serializer/*` | Tie the layers together per format version and build the model. |
| XML serializer | `src/xml/xmlserializer.*` | Reads and writes the model as Microsoft Project compatible XML (MSPDI) — the engine behind [`XmlIO`](../API/XmlIO.md). |
| Model | `src/model/*` | The public value types described under [Data Model](../DataModel/Overview.md). |

## Inside the container

The project data lives under a storage named `   114` (with leading spaces), with one sub-storage per
entity type:

| Storage | Entity |
| :--- | :--- |
| `TBkndTask` | tasks |
| `TBkndRsc` | resources |
| `TBkndAssn` | assignments |
| `TBkndCons` | predecessor links (constraints) |
| `TBkndCal` | calendars |

Each entity stores its fields across a set of streams — fixed-width records (`FixedData`), a
variable-length blob pool (`Var2Data`), and metadata directories (`FixedMeta`, `VarMeta`) — and a
**field map** in the project `Props` describes where each field lives.

Project title and author come from the standard `\x05SummaryInformation` OLE property set, not from
the Microsoft-specific streams.

## Why writing binary `.mpp` is unsupported

Reading binary `.mpp` is hard but tractable. *Writing* it is something essentially no tool other than
Microsoft Project does — there is no reference for producing a byte-valid file, and Microsoft Project
itself does not guarantee stable bytes between saves. MppIO therefore focuses on reading; its
`save()` uses the library's own internal format purely for lossless round-tripping in tests.

For a file Microsoft Project can actually open, the supported output path is **XML**: write the model
as MSPDI with [`XmlIO`](../API/XmlIO.md). Microsoft Project's XML schema is documented and stable, so
unlike the binary format it is a practical interchange target.
