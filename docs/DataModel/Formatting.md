# View and Timeline Formatting

ScheduleIO models selected native Microsoft Project presentation state separately from schedule
entities. The structures are value types, but some carry writer bookkeeping or opaque source data.

```cpp
#include "model/viewstyles.h"
#include "model/usageviewsettings.h"
#include "model/timelineviewsettings.h"
```

Colors are signed `qint32` values in `0xRRGGBB` form. `TextStyle::kAutomatic` and
`TimelineTextStyle::kAutomatic` are `-1`, meaning inherit or use the current theme.

## schedule::TextStyle

| Member | Type | Default | Meaning |
| :--- | :--- | :--- | :--- |
| `bold`, `italic`, `underline`, `strikethrough` | `bool` | `false` | Font emphasis. |
| `color` | `qint32` | automatic | Foreground color. |
| `backColor` | `qint32` | automatic | Cell background color. |
| `backPattern` | `int` | `0` | MPP background pattern; `0` transparent, `1` solid. |
| `fontName` | `QString` | empty | Font-family override; empty inherits the view font. |
| `fontSize` | `int` | `0` | Point-size override; `0` inherits. |
| `fontBaseIndex` | `int` | `-1` | Exact native FONT_BASES index retained for binary preservation. |

`isDefault()` tests whether all values inherit. Semantic equality compares emphasis, colors, and
background pattern; font name/size/index are intentionally excluded because native font-table
normalization can change them without changing the modeled style.

Tasks use `rowFormat` for row-wide text and `cellFormats` for per-column overlays. A built-in column
key is the consuming application's stable numeric column type; custom fields use `c:<field name>`.

## schedule::ViewLineStyle

`color` is automatic or `0xRRGGBB`. `lineStyle` uses native codes: `0` none, `1` solid, `2` dotted1,
`3` dotted2, and `4` dashed.

## schedule::ViewBarStyle

One default Gantt bar-style row. Several members deliberately expose raw MPP values because a stable,
complete friendly enumeration is not yet available.

| Member | Meaning |
| :--- | :--- |
| `name` | Style identity used to match rows, such as Task, Milestone, or Summary. |
| `middleColor`, `startColor`, `endColor` | Middle/start/end colors or automatic. |
| `middleShape`, `middlePattern`, `startShape`, `endShape` | Native shape/pattern bytes; start/end shapes are packed native values. |
| `middleFlag`, `flag87` | Undecoded native flags preserved verbatim. |
| `fromField`, `toField` | Raw MPP field IDs defining the bar span; `0` means unset. |
| `showFor`, `showForNot` | Native criteria bitsets selecting applicable tasks. |
| `row` | Gantt row `1` through `4`. |
| `barText` | Five field-array indices for Left, Right, Top, Bottom, Inside; `-1` means none. |
| `styleId` | Native style identifier. |

## schedule::ViewStyles

| Member | Meaning |
| :--- | :--- |
| `present` | Styles were loaded or edited; writers patch view data only when set. |
| `text[TextCategoryCount]` | Text styles for the categories below. |
| `sheetRows`, `sheetColumns`, `ganttRows` | Gridline styles. |
| `currentDateLine`, `statusDateLine` | Date-line styles. |
| `barStyles` | Default Gantt bar styles in native order. |

`TextCategory` values, in native order, are Highlighted, RowAndColumn, NonCritical, Critical,
Summary, Milestone, MiddleTimescale, BottomTimescale, BarTextLeft, BarTextRight, BarTextTop,
BarTextBottom, BarTextInside, Marked, ProjectSummary, External, and TopTimescale.

`bar(name)` finds or creates a case-insensitive named row. `taskBar()`, `milestone()`,
`summaryBar()`, and `projectSummaryBar()` are convenience accessors. Set `present = true` after
editing direct members so the writer knows to apply them.

## schedule::UsageTableColumn

| Member | Type | Meaning |
| :--- | :--- | :--- |
| `fieldId` | `quint32` | Native Microsoft Project field ID. |
| `width` | `int` | Native byte-sized column width, clamped to `0`–`255` by setters. |
| `title` | `QString` | Optional displayed heading. |

## schedule::UsageViewSettings

Used for Gantt, Task Usage, Resource Usage, and Team Planner presentation state.

| Member | Default | Meaning |
| :--- | :--- | :--- |
| `present` | `false` | Native settings were read or have been supplied. |
| `modified` | `false` | Writer patch request; excluded from semantic identity. |
| `viewName`, `tableName` | empty | Native view and table names. |
| `columns` | empty | Ordered table columns. |
| `detailFields` | empty | Ordered native Usage-view detail field IDs. |
| `detailSelection` | empty | Exact application detail selection when native IDs are insufficient. |
| `timescaleSize` | `100` | Timescale enlargement percentage, setter-clamped to `25`–`255`. |
| `tableWidth` | `0` | Left pane width, setter-clamped to `0`–`65535`. |

Use `setColumnWidth()`, `setTimescaleSize()`, `setDetailFields()`, `setDetailSelection()`, and
`setTableWidth()` rather than assigning when possible; each validates input and sets both flags.
`columnWidth()` returns a field's current width or a caller-supplied fallback.

## Timeline structures

### schedule::TimelineBar

| Member | Default | Meaning |
| :--- | :--- | :--- |
| `id` | `0` | Bar ID; 0 is Project's hidden/default row and positive IDs are visible bars. |
| `label` | empty | Optional displayed label. |
| `useCustomDates` | `false` | Use `customStart`/`customFinish` instead of member-derived bounds. |
| `customStart`, `customFinish` | invalid | Optional custom `QDate` bounds. |

### schedule::TimelineItem

| Member | Default | Meaning |
| :--- | :--- | :--- |
| `guid` | empty | Native member GUID. Preserve it for existing rows. |
| `taskUid` | `-1` | Referenced task unique ID. |
| `barId` | `1` | Owning `TimelineBar::id`. |
| `onTimeline` | `true` | Member is enabled on the timeline. |
| `milestone` | `false` | Member represents a zero-duration milestone. |
| `display` | `Bar` | `TimelineItemDisplay::Bar`, `Callout`, or `TextOnly`. |

### schedule::TimelineTextStyle

| Member | Meaning |
| :--- | :--- |
| `id` | Native style ID. |
| `type` | Timeline text category `0`–`11`, or override type for IDs 12+. |
| `color` | `0xRRGGBB` or automatic/theme. |
| `fontName`, `fontSize` | Font family and point size; zero size is unset. |
| `bold`, `italic`, `underline`, `strikethrough` | Font emphasis flags. |

### schedule::TimelineViewSettings

| Member | Default | Meaning |
| :--- | :--- | :--- |
| `present`, `modified` | `false` | Source presence and writer patch request. |
| `viewUid` | `-1` | Native timeline view record ID. |
| `viewName` | empty | Native view name. |
| `dfltTLView` | `true` | Native default-timeline flag. |
| `bars`, `items`, `textStyles` | empty | Typed timeline contents. A decoded view includes hidden bar ID 0. |
| `dateFormat` | `255` | Native timeline date-format code. |
| `numTextLines` | `1` | Text lines allocated to an item. |
| `showTodayLine`, `showTimescale`, `showPanZoom`, `showDates`, `showTaskProgress`, `showOverlaps` | `true` | Modeled timeline options. |
| `rawXml` | empty | Exact UTF-16LE `<TLViewData>` bytes for preserving unmodeled XML. |

`bar(id)` returns a pointer to a bar or null; `contains(taskUid)` checks membership. Call `touch()`
after direct edits—it sets `present` and `modified`. The writer edits modeled fields into `rawXml` so
unrecognized formatting and attributes survive. Equality compares modeled fields, not flags or raw
bytes; `Project::operator==` omits the timeline member as presentation state.

## Persistence boundaries

View formatting is largely MPP-specific and is not represented by MSPDI. Some per-task and report
format fields currently have limited native write support. Preserve opaque buffers and consult
[Field Coverage](../Reference/FieldCoverage.md) before treating a presentation edit as portable.
