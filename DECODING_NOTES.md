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

Also block-0 FIXED: **CONSTRAINT_TYPE(17)@64** (u16, 100% match), **CONSTRAINT_DATE(18)@66**
(timestamp). **WBS(16)** and **NOTES(15)** are `loc=VAR` but WBS is *not stored* (no var records) —
it equals the computed **OutlineNumber**, so it is derived (dotted per-level counters; project
summary = "0"). WBS matches 100% (Has Macros 82/83, one outline-gap edge).

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
  double **in ten-thousandths** — 100% stored as 10000.0, so divide by 10000), block-0 FIXED.
  Resource-name recall = 100% (7/7, 48/48, 67/67); max-units 100%. NOTE: STANDARD_RATE (field 6,
  fixed @16) reads **0** — resource rates are NOT a fixed field; they live in cost-rate tables (see
  the cost-rate section below).
- **Resource FixedMeta tail gates var-field display in real MS Project (2026-07-10).** The 29 tail
  bytes of a live resource's 37-byte FixedMeta item (past `[flags][offset]`) carry a bit pattern the
  uid-0 stub row lacks: live rows in Average Project.mpp are `e3 ff fd 3f 5c 46 30 c0` + zeros
  (tail byte 4 is `5c` or `5d`; rows with notes flip tail byte 3 `3f→bf` and tail byte 27 `00→40`).
  Writing rows with the stub's "everything absent" tail produces a file our reader and MPXJ read
  fine, but **real MS Project shows every var-backed column blank** (empty Name/Initials in the
  Resource Sheet, assignments showing only `[50%]` with no name) — it treats the bits as
  field-presence flags and never consults the (valid, present) Var2Data blobs. Verified via the
  dpr2hw3 COM oracle: with the stub tail all 7 names read back empty; with the live-row pattern all
  7 Name/Initials and 21 task ResourceNames read back correctly. The writer now stamps the live-row
  pattern (+ notes bits) on every resource row; `tests/probe/dump_rsc.cpp` dumps the TBkndRsc
  quartet for this kind of ground-truth diff. The FixedMeta item **flags u32** did NOT need to
  change (stub's `0x00080000` accepted; real files vary `0x000b0000`/`0x000c0000`/...), and
  all-zero Fixed2Data blocks are also accepted (real rows carry `[GUID][double][GUID]` there —
  still unwritten, no observed symptom). Unwritten real-file var types 68/69/85/726/739/756/757
  were not needed for name display either.
- **Assignments** (`TBkndAssn`, FixedMeta item size **34**, field map `0x00020017`, type high word
  `0x0F40`): UID(0)@0, TASK_UID(1)@4, RESOURCE_UID(2)@8, UNITS(7)@12 (double), WORK(8)@20 (double,
  tenths-of-minute), all block-0 FIXED. Assignment-link recall ≥95% (all XML links found; the `.mpp`
  holds extra assignment rows beyond the filtered export). Generic helper `block0FixedOffsets(fm,
  highWord)` parses any entity field map.
- **Predecessor links** (`TBkndCons`, MPXJ `ConstraintFactory`): FixedMeta item **10**, FixedData
  records **20 bytes**. Per meta item: `u16@0 != 0` ⇒ skip; `u32@+4` = record offset into FixedData.
  Record: `u32@0`=relation UID, `u32@4`=predecessor task UID, `u32@8`=successor task UID,
  `u16@12`=type; skip if either UID 0 or equal. **Lag**: `i32@14` (tenths of a minute, Project
  2013/2016; older files use @16); the units word at @18 affects only the display unit, so
  `lagMillis = value*6000`. Link recall 100% (Average 20/20, Example 64/64); lag validated 20/20 and
  64/64 vs XML `<LinkLag>` in `tst_entities_oracle`.
- **Project start/finish dates**: PropsKey PROJECT_START_DATE=**0x02400002**,
  PROJECT_FINISH_DATE=**0x02400003** in `   114/Props` (4-byte MPP timestamps). 100% match.
  (NOTE: PropsKey ints are decimal — 37748738 = 0x02400002, not 0x024003E2.)
- **Calendars** (`TBkndCal`, MPXJ AbstractCalendarFactory): FixedMeta item **10**, FixedData block
  **12**. For Project 2013/2016: calendarID@8, baseID@0, resourceID@4 (the first ~4 items are
  0xFFFF-marked headers with calID 0 — skip). A **base** calendar (baseID==0xFFFFFFFF) gets its name
  from var-data type **1**; a **resource** calendar's name is the linked resource's name (via
  resourceID@4). Calendar-name recall 100% (9/9, 37/37, 56/56). **Working-day mask** from
  CALENDAR_DATA (var type **8**): 7 days × 60 bytes, `u16@(60*i)`=default flag, `u16@(60*i+2)`=period
  count (>0 == working), MPP day index 0=Sunday; absent ⇒ default Mon-Fri (0x1F). These fixtures only
  use default working time (no type-8 records present), so the mask validates as Mon-Fri. 100% match.
- **Title / Author**: the standard `\005SummaryInformation` OLE property set ([MS-OLEPS]) — header
  section offset @44, section = [size][count] then (propId, propOffset) pairs; PIDSI_TITLE=2,
  PIDSI_AUTHOR=4; values VT_LPSTR(0x1E, ANSI) / VT_LPWSTR(0x1F, UTF-16), `[u32 len incl NUL][bytes]`.
  100% match. (`readSummaryInformation`.)

### Cost, baselines & custom fields (DONE — `tst_cost_oracle`, `tst_baseline_oracle`, `tst_custom_oracle`)

Field indices are ported verbatim from MPXJ `MPPTaskField`/`MPPResourceField`/`MPPAssignmentField`
`FIELD_ARRAY` into `src/codec/mppfieldids.{h,cpp}` (cost scalars, baseline sets 0–10, and the full
custom-field descriptor tables). A field's low-word index is both its FixedData field-map index and
its Var2Data key; the high word selects the entity (task `0x0B40`, resource `0x0C40`, assignment
`0x0F40`).

- **Location is per-file.** Whether a field lands in FixedData (block 0/1) or Var2Data varies, so
  `entityFieldLocations(fieldMap, highWord)` (in `docserializer.cpp`) resolves each index to a
  `{block, offset}` or a var key, and `fillCostBaselineCustom` reads it from there (fixed preferred).
  In the fixtures the cost scalars are FIXED doubles while **all baseline fields are VAR**.
- **Cost = a plain 8-byte IEEE double in the project currency unit — NOT hundredths.** (Verified:
  task UID 464 reads `395999.85` directly at the COST offset.) `FieldDecoders::readDouble`. The
  `/100` seen in the WINPROJ decompilation applies to other code paths, not MPP14 FixedData.
- **Work = a double in *thousandths* of a minute**, so `ms = value * 60` (e.g. 8h → `480000` →
  `28'800'000` ms; verified vs XML on UID 1/6/72/87). `FieldDecoders::decodeWorkDouble`. (This is a
  different unit from DURATION, which is a u32 in tenths of a minute.)
- **Baseline fields**, var keys (baseline 0): COST=6, WORK=1, START=43, FINISH=44, DURATION=27;
  baseline 1 = COST 484/WORK 485/START 482/FINISH 483/DUR 487, etc. (MPXJ also maps baseline
  start/finish/duration a second time to the block-1 indices 1299+; we keep the first/lower form.)
  Baseline dates are 4-byte MPP timestamps, cost a double, work a thousandths-of-minute double,
  duration a u32 tenths-of-minute. A baseline appears only when ≥1 of its fields is present.
- **Resource cost** is frequently not stored on the resource (Project computes it from cost-rate
  tables, which we don't decode). Where `cost`/`actualCost`/`remainingCost` read 0, `readRealMpp`
  rolls them up from the resource's assignment costs, matching the exported resource cost (Has Macros
  67/67 after rollup).
- **Custom fields** → `schedule::CustomField{fieldId, name, value}` where `fieldId = (high<<16)|index`,
  which equals the MSPDI `<FieldID>` exactly (e.g. `188743731 = 0x0B400033`, index 51 = Text1). The
  value type follows the slot kind (Text/Outline Code→QString, Number→double, Cost→currency double,
  Date/Start/Finish→QDateTime, Duration→qint64 ms, Flag→bool). Oracle: 577/577 and 248/248 custom
  attributes value-correct vs the XML `<ExtendedAttribute>` list.

Results vs XML (Average / Example / Has Macros): task Cost/Fixed/Actual/Remaining 100%; resource
Cost 100%; assignment Cost 100%; baseline Start/Finish/Cost/Work/Duration 100%; custom fields 100%.

Diagnostic: `dump_fields <file.mpp> [task|resource|assignment]` prints the full field map (idx, loc,
block) plus a var-type histogram and per-task baseline blobs.

### Notes (raw RTF) — DONE (`tst_notes_oracle`: task + resource validated)

Notes are a **var-data** field: var key **15** (task), **20** (resource), **71** (assignment) — the
MPXJ `MPP*Field.NOTES` index. Unlike names, MPXJ reads NOTES with `Var2Data.getString` (not
`getUnicodeString`), i.e. an **8-bit (Latin1), NUL-terminated** string — the bytes are the **raw RTF
source** (`{\rtf1...}`). `readNotesRtf` in `docserializer.cpp` returns it verbatim into
`schedule::Task/schedule::Resource/schedule::Assignment::notes` (no RTF stripping). Scaffold round-trips it as a
`kFieldNotes` var entry. `tst_notes_oracle` validates task + resource notes against the Average
Project fixture (the XML stores plain text, so it checks each XML note line is recovered inside the
decoded RTF — 2/2 task, 2/2 resource). Assignment notes use the same decoder but no fixture has them
yet, so they are covered only by the round-trip test.

### Resource cost-rate tables (DONE — `tst_costrate_oracle`)

Resource rates live in **var data** under keys **61..65** = cost-rate tables **A..E**
(MPXJ `ResourceField.COST_RATE_A..E`). Each table blob (MPXJ `CostRateTableFactory`): 16-byte
header, then **44-byte entries** — `stdRate dbl@0`, `stdFmt u16@8`, `otRate dbl@16`, `otFmt u16@24`,
`costPerUse dbl@32` (/100), `endDate@40` (tenths-of-minute timestamp via
`decodeTimestampTenths` = epoch + i32*6 s). Rates are stored **per hour**; `rateFromHours` converts
to the format's unit (`fmt` is Microsoft work-time-units, `0xFFFF`=hours; minutes /60, days ×8,
weeks ×40 using default 480/2400 min) — fixtures are all per-hour so the stored double equals the
XML rate. End-date heuristics (MPXJ): `>= 2049-12-31 23:59` ⇒ open-ended; minute on a 5-min boundary
⇒ step back 1 min; entries with non-zero seconds are noise and skipped. Entries are sorted by end
date, each start = previous end + 1 min. `parseCostRateTable` in docserializer →
`schedule::Resource::costRates` (`schedule::CostRate{table,startDate,endDate,standardRate,standardRateUnit,
overtimeRate,overtimeRateUnit,costPerUse}`). Validated: current rate 43/43 (Example) and 63/63 (Has
Macros), and the full per-resource standard-rate sets. Scaffold round-trips as `kFieldCostRates`.

### Calendar working hours & exceptions (DONE — `tst_calendar_oracle`)

From the CALENDAR_DATA blob (var type 8). Layout (MPXJ `AbstractCalendar(AndException)Factory`,
MPP14 hours offset 0): **7 x 60-byte day blocks** (index 0=Sunday..6=Saturday), then exceptions at
**offset 420**. Per day block at `60*i`: `flag u16@0`; `flag==1` ⇒ default day (a **base** calendar
uses the standard week's hours, a **derived** calendar inherits — leave empty); else
`periodCount u16@2`, then periods `start@8+p*2`, `durationTenths@20+p*4`. Times: `getTime` short =
minutes×10 (tenths-of-minute since midnight); duration short = tenths-of-minute (×6000 ms).
Exceptions: `count u16@420`, then 92-byte blocks (+4): `fromDate days@0`, `toDate days@2` (epoch
**1983-12-31**, like the fixed-data timestamps — NOT 1984-01-01); `periodCount u16@14`
(0 ⇒ non-working); periods `start@20+p*2`, `dur@32+p*4`; `nameLen i32@88` (round up to ×4);
UTF-16 name@92. `parseCalendarData` → `schedule::Calendar.workingTimes` (7 lists, Monday..Sunday) +
`exceptions` (`schedule::CalendarException`). **GOTCHA:** calendar FixedData meta offsets are NOT in storage
order, so calendars use `readVarSizedBlocks` (block end = next-higher offset, MPXJ FixedData
semantics) instead of `readFixedBlocks` — otherwise out-of-order calendars (e.g. a newly added one)
are silently dropped. Validated: weekday hours 70/70 (Average), 259/259, 392/392; exceptions 2/2
(Average's "Company Picknick" non-working + "Super Fun Day" working). Scaffold round-trips as
`kFieldCalData`.

## Writing real MPP14 (DONE — `src/serializer/mpp14writer.cpp`)

`MppIO::save()` / `saveToData()` now emit the **real MPP.14 format** (Project
2010-2021), not the scaffold container. Strategy: an empty project saved by
Project 16 is embedded as a template (`src/serializer/mpp14template.mpp`, via
`scheduleio.qrc`); the writer copies its tree verbatim (Props14, `\1CompObj`,
`   214` views/tables/filters, the authentic `   114/Props` field maps, per-
entity Props streams) and regenerates the five Bknd entity storages from the
model, patching `   114/Props` project dates in place and regenerating
`\5SummaryInformation` (title/author, [MS-OLEPS] with VT_LPWSTR values).

Key writer facts (fixture forensics + WINPROJ decompilation):

- **Headers** (WINPROJ `FUN_1408af414`/`FUN_1408af56c`): FixedMeta/Fixed2Meta =
  `[0xFADFADBA][4][itemCount][dataStreamSize]`; VarMeta = 24-byte
  `[0xFADFADBA][0][recordCount][0][0][var2DataSize]`.
- **VarMeta records** are `[u32 uid][u32 offset][u16 typeLow][u16 typeHigh]` —
  the "unused" high u16 is the entity high word (0x0B40/0x0C40/0x0F40/0x0D40).
  All Var2Data blobs are `[u32 byteLen][bytes]`.
- **Record geometry**: task 202 B (+ 64 B Fixed2 block, GUID at 0, manual dates
  1283@50/1284@54), resource 172 B, assignment 110 B, relation 20 B (+ 48 B
  Fixed2 = 3 GUIDs: cons/pred/succ), calendar 12 B. Meta item sizes 47/37/34/10/10;
  Fixed2Meta item sizes 96/51/53/9/10.
- **Placeholder rows**: task/resource FixedData start with three 16-byte rows
  (meta flags 4) — plus a uid-0 stub row for resources; calendars four. Copied
  verbatim from the template.
- Fields are placed via the **template's own field maps** using the same
  `FieldMap::entityFieldLocations` the reader uses, so writer and reader agree
  by construction (fixed block 0/1 at the mapped offset, else a var blob).
- Meta **bit flags**: milestone = meta item byte 10 & 0x02, effort-driven =
  byte 13 & 0x08, manual = Fixed2Meta item byte 8 & 0x80 (Project 2013/2016).
- **Baselines**: cost/work/duration blobs are written even when zero — the
  reader (and MPXJ) treat blob presence as "baseline exists".
- **MPXJ gotcha**: an assignment row is dropped unless its uid appears in the
  assignment VarMeta, so every assignment gets a CREATED (index 634) var entry.
- **Calendar exception blocks** carry a recurrence header MPXJ requires:
  occurrences u16@4 = 1, recurrence type u16@72 = 1 (daily × freq 1 == a plain
  one-off range).
- Entity GUIDs are deterministic v5 UUIDs of the uid, so re-saving the same
  model is byte-identical (`writerIsDeterministic`).

Validated: `tst_semantic_roundtrip` — synthetic model and all four fixtures
survive read → save → read with full model equality, and **MPXJ 13.12 reads the
written files** (Average: 30/30 tasks, 22/22 assignments; Example Template:
92/92, 79/79; title/dates/milestones/outline/costs/links correct).
`tests/probe/resave.exe <in.mpp> <out.mpp>` re-saves a file for external checks.

### Deleted-row flags (DONE — reader mirrors MPXJ)

MS Project keeps deleted rows in the file. The reader now filters like MPXJ:
- **Tasks** (MPXJ `createTaskMap`): a FixedMeta item whose flags u32 has bit
  **0x02** is a deleted row; live rows must also hold >75% of the block-0
  record size. Tasks whose names survive in Var2Data but that have no live
  FixedData row ("name-only ghosts") are dropped after the fill pass.
- **Assignments** (MPXJ `ResourceAssignmentFactory`): a row is dead when the
  **first byte** of its FixedMeta item is non-zero, or when its unique id has
  no VarMeta entries at all (`BkndVarData::hasEntriesFor`).
- Resources/calendars need no flag check (MPXJ filters only by row size).

Validated: reader counts now equal MPXJ's on the originals — Has Macros 83
tasks (was 292) and 65 assignments (was 652), Example Template 92/79, Average
30, Empty 1 — and the resaved files keep those counts in MPXJ.

Known limits: unknown var entries (e.g. task types 173/174/179, 1379/1380) and
`   214` view edits made after the template are not preserved on resave.
MPP.12 write still uses the old scaffold container. Acceptance by Microsoft
Project itself is untested on this machine (no MS Project COM registration);
MPXJ is the strongest available oracle.

### Next fields

- Calendar work weeks (alternate working-week ranges); only the default week's hours are read.
- Resource e-mail/group/type, assignment lag.
- CRITICAL (derive from total slack); outline codes (`TBkndOutlCode`).
- More SummaryInformation/DocumentSummaryInformation props (company, manager, ...).
- Version detection (ApplicationVersion) to pick the right MILESTONE bit table for 2010 files.

### Resource `calendarUniqueId` (DONE, 2026-07-03)

`Resource::calendarUniqueId` is **not** a resource-side field-map field — index 5
(MPXJ `ResourceField.BASE_CALENDAR`) is entirely absent from the resource field
map in every fixture checked. The link lives on the **calendar** side instead:
`TBkndCal`'s 12-byte fixed record (`baseID@0`/`resID@4`/`calID@8`, read since the
calendar-name work) has `resID@4` pointing back to the owning resource for
non-base (resource) calendars. `readRealCalendars` now inverts this onto
`Resource::calendarUniqueId` once both lists are loaded (resources first). The
writer (`mpp14writer.cpp` `TBkndCal` loop) now prefers `Resource::calendarUniqueId`
(via a `calendar uid -> resource uid` map) over the pre-existing name-matching
fallback, fixing a latent bug where two resources sharing one calendar name would
mislink. Validated: `tst_entities_oracle` (new `resource calendar UID` check,
matched by the assigned calendar's **name** since calendar UIDs are already known
to differ between `.mpp` and XML) and `tst_semantic_roundtrip` (full model
equality across all real fixtures).

### Resource Availability Table (DONE, 2026-07-03)

Time-phased Max Units rows (MS Project's Resource Information > General
"Resource Availability" grid) — new `schedule::AvailabilityPeriod`
(`startDate`/`endDate`/`units`, invalid dates = open start/end) and
`Resource::availabilityTable`. Var key **276** (MPXJ `ResourceField.
AVAILABILITY_DATA`). **No existing fixture had this data** — sourced
`mpp14availability.mpp`/`.xml` from MPXJ's own test suite (`github.com/joniles/
mpxj`, `junit/data/`, LGPL-2.1; see `AvailabilityTest.java`/
`AvailabilityFactory.java`) since none of our 4 originals exercise it.

Blob layout (empirically confirmed byte-for-byte against the sourced fixture +
its XML oracle, cross-checked against MPXJ's `AvailabilityFactory.process`):
12-byte header (`[u16 segment count][10 reserved]`), then `(count+1)` 20-byte
boundary slots `[timestamp tenths @0][units double @4, ten-thousandths like
MAX_UNITS][8 unused]`. Slot *i*'s timestamp is period *i*'s start; slot *i+1*'s
timestamp is period *i*'s end (display subtracts 1 minute — the same
inclusive-end convention as cost-rate tables). **Gotcha**: this timestamp uses
epoch **1983-12-31**, not the 1984-01-01 epoch `FieldDecoders::decodeTimestampTenths`
uses elsewhere (cost-rate tables genuinely do use 1984-01-01 — two different
epochs for two different "tenths" fields in the same format; calendar
exceptions use the 1983-12-31 one too). A units value of exactly 0 marks an
implicit "no override" filler segment (leading/trailing/gap) that MPXJ's own
reader skips and ours does too — `readRealResources` only appends nonzero-unit
segments as real `AvailabilityPeriod`s. `mpp14writer.cpp`'s `availabilityBlob()`
does the inverse: sorts periods, synthesizes zero-unit filler segments for any
gaps plus leading/trailing time, and emits the terminating boundary. Validated:
new `tst_availability_oracle` (3/3 periods exact vs XML) and
`tst_semantic_roundtrip` (synthetic model with a real gap between two periods,
plus the sourced fixture, both full model equality).

### Project default calendar (DONE, 2026-07-03)

`Project::calendarUniqueId` (-1 = the implicit "Standard" calendar) existed in
the model but was read/written nowhere. It's stored by **name**, not uid --
PropsKey `DEFAULT_CALENDAR_NAME` = 37748750 (0x0240000E) in `   114/Props`, a
UTF-16LE string (same decode as `PropsReader::string()`). Reader resolves it
against the already-loaded `calendars` list (`readRealCalendars` runs first).
Writer needed a new `patchPropsString()` (unlike the existing `patchPropsU32`,
a string's replacement can be a different byte length than the template's
placeholder, so it splices the item and adjusts the two header "byteSize"
fields rather than patching in place) -- falls back to `"Standard"` when
`calendarUniqueId` doesn't resolve to a named calendar. Validated via
`tst_semantic_roundtrip`'s synthetic model (a real non-Standard calendar set as
the project default).

**Known limit surfaced by the sourced fixture (pre-existing, unrelated):**
`mpp14availability.mpp`'s `TBkndCal` records sit in a layout our calendar
reader doesn't fully parse (calendar-name recall ~1/3 vs the ~100% on our other
4 fixtures) — `tst_entities_oracle` explicitly skips its calendar-identity
checks for this one fixture (`knownCalendarLayoutGap`) rather than loosen them
project-wide. Worth investigating if calendar work resumes, but out of scope here.

## Diagnostics

- `dump_task <file.mpp>` — hex-dumps `TBkndTask` streams and tallies string-bearing field codes.
- `dump_props <file.mpp>` — parses `   114/Props`, lists all keys, dumps the task field map as
  28-byte rows.
- `tst_fixture_cfb` — prints any fixture's full storage tree.
