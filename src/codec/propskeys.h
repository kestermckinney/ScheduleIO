// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef PROPSKEYS_H
#define PROPSKEYS_H

#include <QtGlobal>

// Named ids for the project-level "   114/Props" container (see PropsReader).
// The low 16 bits are the property id; the high bits encode the data type.
//
// The first group predates this file. The second group -- the MS Project
// File > Options settings ScheduleVault's Options dialog edits -- was
// reverse-engineered 2026-08-30 by diffing single-option .mpp files saved from
// real MS Project 2016 on dpr2hw3 against a baseline (see the plan and
// DECODING_NOTES.md; probe generator + diff in that session's scratchpad).
//
// A key left at kUnknown is skipped by both docserializer's reader and
// mpp14writer's writer -- it round-trips through MSPDI only. Four options were
// CONFIRMED (2026-08-30, full-CFB stream diff of single-option MS Project saves)
// to have no field in "   114/Props" and stay kUnknown by nature:
//   - newTasksManual        -- no MS Project COM setter; UI/registry only
//   - autoLinkTasks         -- toggling it changed no project stream
//   - honorConstraints      -- toggling it changed no project stream
//   - showProjectSummaryTask-- per-view display flag; lives in the Gantt view's
//                              "   214/CV_iew" data, not a project property
namespace PropsKey {

// "no id" -- read and write both skip a key set to this.
inline constexpr quint32 kUnknown = 0u;

// ---- predating this file --------------------------------------------------
inline constexpr quint32 ProjectStartDate      = 0x02400002u; // PROJECT_START_DATE (MPP timestamp)
inline constexpr quint32 ProjectFinishDate     = 0x02400003u; // PROJECT_FINISH_DATE (MPP timestamp)
inline constexpr quint32 ScheduleFrom          = 0x02400004u; // SCHEDULE_FROM (u16)
inline constexpr quint32 Title                 = 0x02400008u; // (UTF-16LE)
inline constexpr quint32 MultipleCriticalPaths = 0x02400039u; // MULTIPLE_CRITICAL_PATHS (u16)
inline constexpr quint32 StatusDate            = 0x02400045u; // STATUS_DATE (MPP timestamp)
inline constexpr quint32 DefaultCalendarName   = 37748750u;   // DEFAULT_CALENDAR_NAME (UTF-16LE, by name)

// ---- Options dialog: reverse-engineered 2026-08-30 -----------------------
// Schedule
inline constexpr quint32 NewTasksAreManual          = kUnknown;    // app-level, not in Props
inline constexpr quint32 NewTaskStartIsProjectStart = 0x02400017u; // u16: 0 = project start, 1 = current date
inline constexpr quint32 DefaultTaskType            = 0x02400018u; // u16: 0=Fixed Units,1=Fixed Duration,2=Fixed Work
inline constexpr quint32 DefaultDurationUnits       = 0x02400015u; // u16 PjUnit (7 = days)
inline constexpr quint32 DefaultWorkUnits           = 0x02400016u; // u16 WorkFormat (2 = hours)
inline constexpr quint32 NewTasksEffortDriven       = 0x02400032u; // u16 bool
inline constexpr quint32 AutoLink                   = kUnknown;    // app-level, not in Props
inline constexpr quint32 SplitInProgressTasks       = 0x0240001Au; // u16 bool (default 1)
inline constexpr quint32 HonorConstraints           = kUnknown;    // not found in Props
inline constexpr quint32 CriticalSlackLimit         = 0x02400014u; // u32, whole days
// Calendar -- times are u16 tenths-of-a-minute since midnight
inline constexpr quint32 WeekStartDay              = 0x02400025u;  // u16: 0=Sunday..6=Saturday (== MSPDI)
inline constexpr quint32 FiscalYearStartMonth      = 0x0240002Cu;  // u16: 1..12
inline constexpr quint32 FiscalYearUsesStartYear   = 0x02400041u;  // u16 bool
inline constexpr quint32 DefaultStartTime          = 0x0240001Cu;  // u16 tenths-of-minute (480 min => 4800)
inline constexpr quint32 DefaultEndTime            = 0x02400021u;  // u16 tenths-of-minute (1020 min => 10200)
inline constexpr quint32 MinutesPerDay             = 0x0240001Du;  // u32
inline constexpr quint32 MinutesPerWeek            = 0x0240001Eu;  // u32
inline constexpr quint32 DaysPerMonth             = 0x0240138Fu;   // u16
// Calculation options for this project
inline constexpr quint32 MoveCompletedEndsBack     = 0x024013A0u;  // u16 bool
inline constexpr quint32 MoveRemainingStartsBack   = 0x024013A1u;  // u16 bool
inline constexpr quint32 MoveRemainingStartsForward = 0x024013A2u; // u16 bool
inline constexpr quint32 MoveCompletedEndsForward  = 0x024013A3u;  // u16 bool
inline constexpr quint32 UpdatingTaskStatusUpdatesResourceStatus = 0x02400019u; // u16 bool (default 1)
// Financial
inline constexpr quint32 CurrencySymbol           = 0x02400010u;   // UTF-16LE
inline constexpr quint32 CurrencySymbolPosition   = 0x02400011u;   // u16: 0..3
inline constexpr quint32 CurrencyDigits           = 0x02400012u;   // u16
inline constexpr quint32 CurrencyCode             = 0x024013BBu;   // UTF-16LE
inline constexpr quint32 DefaultStandardRate      = 0x0240001Fu;   // IEEE double 8B (item flags 0x09)
inline constexpr quint32 DefaultOvertimeRate      = 0x02400020u;   // IEEE double 8B (item flags 0x09)
inline constexpr quint32 DefaultFixedCostAccrual  = 0x02400047u;   // u16: 1=Start,2=End,3=Prorated
inline constexpr quint32 EarnedValueMethod        = 0x024013A9u;   // u16: 0=% Complete,1=Physical % Complete
inline constexpr quint32 BaselineForEarnedValue   = 0x024013AEu;   // u16: binary value = MSPDI value + 1 (1 = Baseline)
inline constexpr quint32 ShowProjectSummaryTask   = kUnknown;      // per-view, not in Props

} // namespace PropsKey

#endif // PROPSKEYS_H
