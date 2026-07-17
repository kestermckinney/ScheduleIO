# Microsoft Project (.mpp) Test Sample Set

A diverse, programmatically-generated set of Microsoft Project files for
reverse-engineering / testing MPP readers and writers. Each sample ships as a
matched pair:

- `<name>.mpp` — the native binary (the format under study)
- `<name>.xml` — Microsoft's own MSPDI XML export of the *same* project

Generated on this machine with:

- **Microsoft Project Professional** 16.0 (build 16.0.20131) — Project 2016/2019/O365 family
- **Python** 3.14 with **pywin32** driving the `MSProject.Application` COM object
- Generator: [`generate_samples.py`](generate_samples.py)
- Single-task minimal example: [`create_one_task_project.py`](create_one_task_project.py)

Regenerate everything:

```bash
python generate_samples.py                       # all samples
python generate_samples.py 03_hierarchy_dependencies   # just one (by folder name)
```

---

## ⚠️ The most important finding: fonts are NOT in the XML

Microsoft's MSPDI XML schema is a **data** format. It stores tasks, resources,
assignments, dates, links, baselines, etc. It does **not** store any of the
character/cell **presentation** formatting — font family, size, bold/italic/
underline/strikethrough, text color, or cell background. All of that lives
**only inside the .mpp binary**.

Therefore, for the font samples, the `.xml` is useful only for the task
structure; the **`manifest.json`** in `01_font_cell_formatting/` is the
authoritative ground-truth record of exactly what formatting was applied to
which (row, column).

---

## Samples

| # | Folder | What it exercises |
|---|--------|-------------------|
| 01 | `01_font_cell_formatting` | **Cell-level character formatting** (the headline sample) |
| 02 | `02_text_styles` | **Category text styles** (a *different* formatting mechanism) |
| 03 | `03_hierarchy_dependencies` | Outline hierarchy, all 4 link types + lag/lead, constraints, milestones |
| 04 | `04_resources_assignments` | Work / material / cost resources and assignments |
| 05 | `05_calendar_durations` | Duration units (min→weeks), elapsed duration, calendar exception |
| 06 | `06_unicode_notes` | Unicode & special-char names, notes, hyperlink, deadline |
| 07 | `07_baseline_progress` | Baseline snapshot + % complete + actuals |
| 08 | `08_edge_empty` | Edge case: a valid project with **zero** tasks |
| 09 | `09_edge_milestone_only` | Edge case: a single milestone |
| 10 | `10_cell_background` | **Cell background fills** (CellColor + Pattern) |

Samples 01–03 and 10 include a `preview.png` screenshot of the file open in
Project; 01 and 10 also ship a `manifest.json` recording exactly what
formatting was applied to which row (their authoritative ground truth —
`tst_format_oracle` checks the decoded model against it).

### 01 — Cell-level character formatting  ← *font emphasis*

18 tasks, each task's **Name** cell formatted directly (Format ▸ Font) via
`Application.Font32Ex`. Covers 8 font families (Arial, Times New Roman, Calibri,
Verdana, Georgia, Courier New, Comic Sans MS, Tahoma, Trebuchet MS, Segoe UI,
Wingdings), sizes 6→36 pt, bold, italic, underline, strikethrough and all
combinations, plus text colors including a custom magenta/teal/olive RGB. See
`manifest.json` for the exact per-row spec.

### 02 — Category text styles

The *other* way Project applies fonts: **Format ▸ Text Styles**, which styles a
whole **class** of rows (all summary tasks, all milestones, all critical tasks,
marked tasks, the row/column header) at once, via `Application.TextStylesEx`.
This is stored differently in the binary from per-cell formatting, so both
mechanisms are worth capturing.

---

## COM / format facts discovered while building this (useful for RE)

These are concrete, verified behaviors of this Project build:

**Enums**

- `PjFileFormat`: `pjMPP = 0`. There is **no** XML member — XML is selected via
  the `FormatID` string arg `"MSProject.XML"` to `FileSaveAs`, not via the enum.
- `PjSaveType`: `pjDoNotSave = 0`, `pjSave = 1`, `pjPromptSave = 2`.
- Task link types (`PjTaskLinkType`): `FF = 0`, `FS = 1`, `SF = 2`, `SS = 3`.
- Constraints (`PjConstraint`): `ASAP=0 ALAP=1 MSO=2 MFO=3 SNET=4 SNLT=5 FNET=6 FNLT=7`.
- COM resource type (`PjResourceType`): `Work=0 Material=1 Cost=2`.

**Two different color encodings (important!)**

- **Cell/character color** (`Font32Ex`) takes a full Windows **COLORREF**
  integer: `R + G*256 + B*65536`.
- **Text-style color** (`TextStylesEx`) takes a **16-color palette index**
  (`Black=0 Red=1 Yellow=2 Lime=3 Aqua=4 Blue=5 Fuchsia=6 White=7 Maroon=8
  Green=9 Olive=10 Navy=11 Teal=12 Purple=13 Silver=14 Gray=15`); `255` means
  "automatic". Passing a COLORREF here fails with "argument value is not valid".

**MSPDI XML encodings**

- Resource `<Type>` in XML is **not** the COM enum: XML `Work = 1`,
  `Material = 0`; a **cost** resource is `Type = 0` with `<IsCostResource>1</IsCostResource>`.
- Link `<LinkLag>` is in **tenths of a minute** (e.g. one 8-hour working day of
  lag = `4800`; a one-day lead = `-4800`).

**API gotchas**

- The document object has **no** `TaskDependencies` collection. Create links by
  writing the **`Task.Predecessors`** text field, e.g. `"5SS"`, `"2FS+1d"`,
  `"8FS-1d"`.
- `ResourceNames` does **not** split on `;` under COM (it creates one literal
  resource). Assign with `Task.Assignments.Add(taskUID, resourceUID)` and set
  `.Units` / `.Cost`.
- Set hierarchy with `Task.OutlineLevel = n` (writable). `OutlineIndent()` on a
  multi-row selection is unreliable and can nest siblings into each other.
- `Application.BaselineSave()` with no args opens the interactive **Set
  Baseline** dialog and blocks automation — set `Task.BaselineStart/Finish/
  Duration` directly instead.
- **`SelectTaskField(Row, Column)` is RELATIVE by default** (`RowRelative`
  defaults to True): each call moves the selection `Row` rows *down from the
  current selection*, so loops drift cumulatively (formats intended for rows
  1/2/3/4/5 landed on 2/4/7/11/16 in v1 of sample 01 — MS Project's own
  rendering confirmed the drift). Always pass `RowRelative=False`. This same
  drift was why cell-background fills originally appeared "on the wrong rows"
  and sample 10 was withheld; with absolute selection both work fine.
- **Cell background pattern values** (`Font32Ex` `Pattern` arg = the value
  stored in the .mpp): `0`=transparent, `1`=solid, `2`=light dotted,
  `4`=heavy dotted — matching MPXJ's `BackgroundPattern` enum, verified
  against Project's rendering of sample 10.
- **Set `ActiveProject.Title` explicitly before saving.** Otherwise the XML
  export (saved first, while the document is still the unsaved "ProjectN")
  and the .mpp (saved after, under its file name) disagree on Title and on
  the UID-0 project-summary task's Name for no real reason.

**Automation must run under Windows PowerShell 5.1 / CPython, not `pwsh` 7** —
PowerShell 7's COM interop mis-binds this object model.

---

## Safe COM automation pattern (no orphaned "save?" dialogs)

The generator's `MSProjectApp` context manager guarantees that Project is always
shut down cleanly: on exit (even on exception) it force-closes every open
document with `pjDoNotSave` and then `Quit(pjDoNotSave)` **before** any COM
reference is garbage-collected. This is what prevents Project from popping a
modal "Do you want to save?" dialog and hanging. Any script that drives Project
should do the same — never let a live `Application` object with an open document
reach the garbage collector.
