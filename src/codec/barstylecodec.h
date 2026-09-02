// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef BARSTYLECODEC_H
#define BARSTYLECODEC_H

#include <QByteArray>

namespace schedule { class ViewBarStyle; }

// (De)serialiser for one row of the Gantt Chart view's default bar-style table
// -- a 195-byte STYLE_DATA record (Props9 key 574619656). Offsets and the
// reserved 200-slot array layout are documented in DECODING_NOTES.md
// "Default bar styles (STYLE_DATA)". The model holds the *raw* MPP byte / field
// -id values; friendly-enum translation is Schedule Vault's GanttChartFormat.
namespace BarStyleCodec {

constexpr int kRecordSize = 195;   // bytes per bar-style record
constexpr int kMaxRecords = 200;   // fixed slot count in the STYLE_DATA blob
constexpr int kCountOffset = 2243; // u8 barCount, relative to the blob start
constexpr int kFirstRecord = 2255; // first record, relative to the blob start
// Bytes reserved after the last slot for the blob's trailer (progress-line
// styles + date format). Anchored to the blob end -- never overwrite these.
constexpr int kTrailerBytes = 108;

// Decode the 195-byte record at `styleData[off .. off + kRecordSize)`.
// Out-of-range `off` yields a default-constructed style.
schedule::ViewBarStyle readRecord(const QByteArray &styleData, int off);

// Encode `bar` into the record at `styleData[off ..]`, in place: modelled
// fields are overwritten; bytes with no model field (the record's few reserved
// gaps) are left untouched so a real file's undecoded bytes survive a rewrite.
// The caller guarantees the slot is within `styleData`.
void writeRecord(QByteArray &styleData, int off, const schedule::ViewBarStyle &bar);

} // namespace BarStyleCodec

#endif // BARSTYLECODEC_H
