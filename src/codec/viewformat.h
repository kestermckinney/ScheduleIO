// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef VIEWFORMAT_H
#define VIEWFORMAT_H

#include <QByteArray>

class CompoundFile;
namespace schedule { class Project; }

// Gantt-view formatting codec for `   214/CV_iew` (ported from MPXJ's
// GanttChartView14 / ViewFactory14, github.com/joniles/mpxj):
//
//   CV_iew holds one 138-byte FixedData record per view (view id u32@0,
//   UTF-16 name@4, splitViewFlag u16@110, viewType u16@112; GANTT_CHART = 1)
//   plus VarMeta/Var2Data. A view's var entry of type 6 (PROPERTIES) is a
//   Props9 block: 16-byte header (item count u16@12), then items
//   [u32 size][u32 key][u32 flags][data], each 2-byte aligned.
//
//   Key 574619656 (STYLE_DATA) carries the view-wide template: 32-byte default
//   text styles at offsets 26+32n (fontBase u8@0, bold/italic/underline/strike
//   bits u8@3, colour@4, backColour@16, backPattern u16@28), 30-byte gridline
//   records at 667..1057 (colour@0, lineStyle u8@13), and the default bar
//   styles (count u8@2243, 195-byte records from 2255: middle colour@+2,
//   start colour@+16, end colour@+29, UTF-16 name@+91).
//
//   Key 574619660 (COLUMN_PROPERTIES) carries per-cell "exceptional" text
//   styles (Format > Font): 44-byte records — task uid u32@0, field type
//   u32@4, fontBase u8@8, style bits u8@11, colour@12, backColour@24,
//   backPattern u16@36, change-flags u16@40 (bold 1, underline 2, italic 4,
//   colour 8, font 16, backColour 64, backPattern 128).
//
//   Colours are r,g,b + a flag byte (0 = explicit, nonzero = "Automatic").
namespace ViewFormat {

// Read the Gantt Chart view's formatting into the project: fills
// Project::viewStyles and each task's rowFormat. Missing or short view data
// leaves everything at defaults (viewStyles.present == false).
void read(const CompoundFile &cf, schedule::Project *out);

// Ensure every requested font family/size has a FONT_BASES entry and update
// the corresponding TextStyle::fontBaseIndex values. When the source project
// did not carry a font table, `fallback` (normally the embedded MPP template's
// table) is used as the starting point.
void prepareFontBases(schedule::Project *project, const QByteArray &fallback);

// Whether the project carries any formatting the writer must patch into the
// template's view data (styles present, or any task row/cell formatted).
bool wantsPatch(const schedule::Project &in);

// Whether either native Usage table carries edited column widths.
bool wantsTablePatch(const schedule::Project &in);

// Rebuild the template's CV_iew VarMeta/Var2Data with the project's formatting
// patched into the Gantt Chart view's Props9 block, adding both streams to
// `out`. Returns false (adding nothing) when the template has no usable Gantt
// view; the caller should then copy the template's streams verbatim.
bool patch(const CompoundFile &tpl, CompoundFile &out, const schedule::Project &in);

// Patch the Task/Resource Usage table column widths in `   214/CTable` while
// retaining every other native table byte. Returns false when no matching
// source table can be patched.
bool patchTables(const CompoundFile &tpl, CompoundFile &out, const schedule::Project &in);

// Writes ScheduleIO extension streams that preserve presentation choices with
// no native Microsoft Project representation.
void writeExtensions(CompoundFile &out, const schedule::Project &in);

} // namespace ViewFormat

#endif // VIEWFORMAT_H
