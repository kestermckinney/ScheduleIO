// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef AUTOFILTERCODEC_H
#define AUTOFILTERCODEC_H

// A view's column AutoFilters, as Microsoft Project stores them in the view's
// Props9 properties (item 574619669 / 0x22400015 of the view's CV_iew type-6
// record), and its AutoFilter switch (`   214/Props` item 0x0340000A, u16).
// Decoded from files Project 2016 wrote through SetAutoFilter
// (tests/fixtures/mpp_samples/autofilter, tools/autofilter-samples-remote.vbs
// in ScheduleVault).
//
//   item   u32 0, u32 stamp (MPP date), u32 entryCount, u32 itemLength,
//          entries, u32 0
//   entry  u32 fieldId, u32 entryLength (from the entry's start, through its
//          strings and their u32 0), u32 PjAutoFilterType,
//          criteria (below), then -- for an In filter -- its value list:
//          u16 count, count x (u16 type, 8 payload bytes as a value block's
//          @14, then a UTF-16 z-string for a text item). A listed date's
//          time is 0xFFFF ("any time that day"); 0xFFFFFFFF is NA.
//   criteria  B: u32 0xFFFFFFFF, u32 0, u32 ?, u16 0, u16 blockCount,
//          u32 stringsOffset; blockCount 36-byte blocks from B+0x14, then
//          the text values' UTF-16 z-strings in block order, then u32 0.
//          Block offsets are relative to B; u16@32 first child, u16@34 next
//          sibling (0xFFFF none).
//   blocks  operator: u16@0 test code (0x3E8..) or 0x19 And / 0x1A Or,
//          u32@12 5 for a test; children field then value(s)
//          field: u32@0 7, u32@4 3, u32@8 field id
//          value: u32@0 1, u32@4 8, u16@12 type (8 text, 0x13 date or
//          duration, 2 percent, 5 number / cost / work, 0x0B flag,
//          0x4000 list), value from @14, u16@24 duration units (2 = work)
//          Within / NotWithin (0x3E8 / 0x3F0) take two value children.
//
// Dates are u32 (days since 1984-01-01 << 16 | tenths of a minute); durations
// u32 tenths of a minute; work a double of minutes x 1000; money a double in
// hundredths of the currency unit.

#include "model/viewautofilter.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>

namespace AutoFilterCodec {

constexpr quint32 kPropsKey = 0x22400015u;        // view Props9 item
constexpr quint32 kSwitchKey = 0x0340000Au;       // `   214/Props`, u16 on/off

QList<schedule::ColumnAutoFilter> decode(const QByteArray &item);

// `stamp`: the date Project records with the filters (the status date, or
// today, at the default start time).
QByteArray encode(const QList<schedule::ColumnAutoFilter> &filters, const QDateTime &stamp);

// Whether a field holds money (stored in hundredths in filter values).
bool isCurrencyField(quint32 fieldId);

// MPP date-time <-> local QDateTime ("NA", 0xFFFFFFFF, <-> invalid).
quint32 toMppDate(const QDateTime &dt);
QDateTime fromMppDate(quint32 v);

} // namespace AutoFilterCodec

#endif // AUTOFILTERCODEC_H
