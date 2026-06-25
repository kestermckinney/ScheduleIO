<!-- Copyright (C) 2026 Paul McKinney -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# MPP decoding notes (reverse-engineered from fixtures + validated vs XML)

Running record of the `.mpp` binary format as decoded so far. Validated against the
MS Project XML exports in `tests/fixtures/`. All fixtures are **MPP.14**.

## Container (DONE, in `src/ole/compoundfile.cpp`)

OLE2 / [MS-CFB]. Top level:
- `Props14` — version marker (the suffix `14`/`12` is the format version).
- `\x01CompObj`, `\x05SummaryInformation`, `\x05DocumentSummaryInformation`, `MsoDataStore`.
- `   114` (3 leading spaces) — **main data**.
- `   214` — views/tables/filters (`CTable`, `CFilter`, `CV_iew`, `CReport`, ...).

Under `   114`: entity sub-storages `TBkndTask`, `TBkndRsc`, `TBkndAssn`, `TBkndCal`,
`TBkndOutlCode` (entity list is also stored as a string — see Props key `0x024003e8`).

## Props container (DONE, in `src/codec/propsreader.cpp`)

```
16-byte header: [u32 byteSize][u32 byteSize][u32][u32]
then items:     [u32 dataLen][u32 key][u32 flags][data dataLen bytes]
```
Key low 16 bits = property id; high bits = data type. `   114/Props` is the project-level
props (holds the field maps). Verified: key `0x024003e8` =
`"TBkndTask,TBkndRsc,TBkndCal,TBkndAssn,TBkndCons,TBkndOutlC..."`.

## Entity var data (DONE, in `src/codec/bkndvardata.cpp` — ported from MPXJ VarMeta12/Var2Data)

Per entity: `VarMeta` + `Var2Data`.
- `VarMeta`: magic `0xFADFADBA`, then a **24-byte header**
  `[magic][u32][u32 itemCount][u32][u32][u32 dataSize]`, then **12-byte records**:
  `[u32 uniqueID][u32 offset][u16 type][u16 unused]`. Builds map `(uniqueID, type) → offset`.
- `Var2Data`: blob pool. A string blob at `offset` is `[u32 byteLen incl. UTF-16 NUL][UTF-16LE]`.
- The record `type` is the field's var-data key = `(MPP field type id) & 0xFFFF` (because
  `FieldMap14.useTypeAsVarDataKey()` is true), which equals the `MPPTaskField.FIELD_ARRAY` index.

## Entity fixed data (DONE for dates/duration — MPXJ FixedMeta/FixedData port)

- `FixedMeta`: **16-byte header** `[magic 0xFADFADBA][u32][u32 itemCount][u32]`, then N items of a
  fixed size (tasks: **47 bytes**, passed to MPXJ's `new FixedMeta(stream, 47)`). Each item's `+4`
  field is the **offset of that task's block into `FixedData`**.
- `FixedData`: split into per-item blocks using the FixedMeta offsets (block = data[itemOffset ..
  nextItemOffset]). A task's `uniqueID` is at the block offset given by the field map for UNIQUE_ID.
- Timestamps (MPXJ `MPPUtility.getTimestamp`): at `off` a u16 time in **tenths of a minute**, at
  `off+2` a u16 **days** since **1983-12-31**; value = epoch + days + 6·time seconds. `days<=1` or
  `0xFFFF` ⇒ no date. Codec in `fielddecoders.cpp::decodeMppTimestamp`.
- Duration: a u32 in tenths of a minute (÷600 = hours). `decodeDurationTenthMinutes`.

## Task names (DONE — MPXJ port, 100% recall vs XML oracle)

Task names are var-data field **type 14** (`MPPTaskField.FIELD_ARRAY[14] = TaskField.NAME`). With
the correct `VarMeta12` layout above, `BkndVarData::stringsForType(14)` yields `(uniqueID, name)`
for every task. Validated by `tst_task_names_oracle`:

| Fixture          | XML task names recovered | decoded unique |
|------------------|--------------------------|----------------|
| Average Project  | 30 / 30 (100%)           | 30             |
| Example Template | 92 / 92 (100%)           | 92             |
| Has Macros       | 43 / 43 (100%)           | 65 (*)         |
| Empty            | 1 / 1 (100%)             | 1              |

(*) Has Macros' `.mpp` legitimately contains more tasks than its filtered XML export, so the
oracle measures **recall of the XML names** (every real name recovered), not strict subset.

The earlier "name code is 6 / file-specific" saga was an artifact of a WRONG VarMeta layout (I had
guessed a 32-byte header and `[type][flag][uid][offset]` order). MPXJ's real layout — 24-byte
header, `[uid][offset][type][unused]` — fixes it: NAME is **type 14 universally**, and the old
"code 6 / code 1380" reads were misaligned garbage that coincidentally landed on name blobs.

## Field map (keys + entry layout, from MPXJ FieldMap / FieldMap14 / Props)

- `   114/Props` keys (from `PropsKey`): TASK_FIELD_MAP=`0x00020014`, TASK_FIELD_MAP2=`0x03000014`,
  RESOURCE=`0x00020015`/`0x03000015`, ASSIGNMENT=`0x00020017`/`0x03000017`, RELATION=`0x00020016`.
  (An earlier analysis of `0x03000017` was the **assignment** map — wrong entity.)
- Entry = **28 bytes**: `+0 u32 mask`, `+4 u16 fixedDataOffset` (0xFFFF ⇒ not fixed),
  `+6 u8 varDataKey` (only when `useTypeAsVarDataKey()` is false), `+12 u32 typeValue`,
  `+20 u16 category` (0x0B/0x64 ⇒ META, else FIXED if offset≠0xFFFF else VAR).
- MPP14 `useTypeAsVarDataKey()` is **true** ⇒ `varDataKey = typeValue & 0xFFFF`.
- `typeValue` decodes via `MPPTaskField`: high word `0x0B40` = task base; low word indexes
  `FIELD_ARRAY` (`0x0B400000`→WORK, `0x0B400006`→BASELINE_COST, `0x0B40000E`→NAME(14), 15→NOTES,
  16→WBS, 23→ID, 24→MILESTONE, 27→BASELINE_DURATION, 29→DURATION).

Note: the task-name reader currently hardcodes NAME=14 (universal for MPP12/14 tasks). A full field
map parser is only needed to reach arbitrary fields generically.

## Task dates / duration (DONE — validated by `tst_task_dates_oracle`)

`readRealMpp` parses the task field map (`parseTaskFixedOffsets`) for the block-0 fixed offsets of
UNIQUE_ID(86)@4, DURATION(29)@84, START(35)@96, FINISH(36)@100, splits FixedData into per-task
blocks (`readFixedBlocks`), and fills Start/Finish/Duration. Validated vs the XML oracle:

| Fixture          | Start | Finish | Duration |
|------------------|-------|--------|----------|
| Average Project  | 29/29 | 29/29  | 30/30    |
| Example Template | 92/92 | 92/92  | 92/92    |
| Empty            | 1/1   | 1/1    | 1/1      |
| Has Macros       | 4/4 * | 4/4 *  | 83/83    |

Manual **and** auto scheduling now decode at 100% (table updated above).

### Manual-scheduled tasks (DONE)

MS Project keeps duplicate START/FINISH field-map entries: index 35/36 in **block 0** (FixedData,
auto/computed) and 1283/1284 "Task Start/Finish" in **block 1** (Fixed2Data, manual). The effective
date = block-1 field 1283/1284 **if valid**, else block-0 field 35/36. (MPXJ does the equivalent:
the base START field maps to 1283, overridden by SCHEDULED_START for auto tasks.) `readRealMpp`
reads Fixed2Data via its FixedMeta (adaptive item size {92..96}, derived as `(size-16)/itemCount`)
and applies the fallback. Validated 100% on Has Macros (79 manual + 4 auto).

### Other task fields (DONE — `tst_task_fields_oracle`)

Block-0 FIXED fields, by `FIELD_ARRAY` index → offset: **ID(23)@0**, **PERCENT_COMPLETE(32)@92**,
**OUTLINE_LEVEL(249)@172** (note: outline uses 249, not 85). ID and OutlineLevel match 100% on all
fixtures. PercentComplete matches ~87–100% — summary tasks store a duration-weighted rollup that
differs from the displayed/exported value (leaf tasks are exact).

**META bit-flag + derived fields (DONE):**
- **MILESTONE**: bit in the 47-byte FixedMeta item — `getInt(metaItem, 10) & 0x02` for Project
  2013/2016 (`getInt(metaItem, 8) & 0x20` for 2010). MppBitFlag reads `getInt(data,offset)&mask`.
  Fixtures here are 2016. 100% match. (Version detection via ProjectProperties applicationVersion is
  a refinement if 2010 files appear.)
- **SUMMARY**: derived — a task is a summary if a following task (ID order) is one outline level
  deeper, OR it is the project-summary row (outline level 0). 100% match. (CRITICAL would be derived
  from slack; not yet done.)

### Resources & assignments (DONE — `tst_entities_oracle`)

- **Resources** (`TBkndRsc`, FixedMeta item size **37**, field map `0x00020015`): NAME=var key 1,
  INITIALS=var key 2 (resource type high word `0x0C40`); UID(27)@4, ID(0)@0, MAX_UNITS(4)@8 (8-byte
  double, 1.0==100%), all block-0 FIXED. Resource-name recall = 100% (7/7, 48/48, 67/67).
- **Assignments** (`TBkndAssn`, FixedMeta item size **34**, field map `0x00020017`, type high word
  `0x0F40`): UID(0)@0, TASK_UID(1)@4, RESOURCE_UID(2)@8, UNITS(7)@12 (double), WORK(8)@20 (double,
  tenths-of-minute), all block-0 FIXED. Assignment-link recall ≥95% (all XML links found; the `.mpp`
  holds extra assignment rows beyond the filtered export). Generic helper `block0FixedOffsets(fm,
  highWord)` parses any entity field map.

### Next fields

- More VAR string fields by index (task NOTES=15, WBS=16; resource e-mail, group, ...).
- CRITICAL (derive from total slack), constraints (`TBkndCons`), calendars (`TBkndCal`),
  outline codes (`TBkndOutlCode`).
- Project-level properties (title/author/dates) from the top-level Props.
- Predecessor/successor links (the `RELATION_FIELD_MAP` / task link records).
- Version detection (ApplicationVersion) to pick the right MILESTONE bit table for 2010 files.

## Diagnostics

- `dump_task <file.mpp>` — hex-dumps `TBkndTask` streams and tallies string-bearing field codes.
- `dump_props <file.mpp>` — parses `   114/Props`, lists all keys, dumps the task field map as
  28-byte rows.
- `tst_fixture_cfb` — prints any fixture's full storage tree.
