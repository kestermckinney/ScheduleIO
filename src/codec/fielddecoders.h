// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef FIELDDECODERS_H
#define FIELDDECODERS_H

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUuid>

// Scalar field codecs for the .mpp binary payload. Everything here is
// little-endian (x64 PE) and timezone-stable (UTC) so that decoding a fixture
// produces identical results on Windows, macOS and Linux.
//
// NOTE: the exact wire epoch and duration units must be confirmed against real
// fixtures (see plan, Layer 3 oracle). The functions below are defined as exact
// inverses of each other, which is what the Layer 1 unit tests assert.
namespace FieldDecoders {

// MS Project's documented timestamp epoch.
QDateTime epoch();

// Scaffold timestamp: u32 seconds since epoch() (0xFFFFFFFF == invalid/no date).
// Second precision keeps it lossless for timestamps the reader produces.
QDateTime decodeTimestampSeconds(quint32 seconds);
quint32   encodeTimestampSeconds(const QDateTime &dt);

// MPP fixed-data timestamp (MPXJ MPPUtility.getTimestamp): at `offset` a u16
// time in tenths of a minute, at `offset+2` a u16 day count since 1983-12-31.
// Returns an invalid QDateTime for the "no date" sentinels. UTC for stability.
QDateTime decodeMppTimestamp(const QByteArray &block, int offset);

// Durations are stored as tenths of a minute. We normalise to milliseconds in
// the model so callers never deal with raw units.
qint64 decodeDurationTenthMinutes(qint32 raw);
qint32 encodeDurationTenthMinutes(qint64 millis);

// Percent-complete is stored as an integer 0..100; the model uses a 0.0..1.0 ratio.
double decodePercent(quint16 raw);
quint16 encodePercent(double ratio);

// 16-byte GUID <-> QUuid.
QUuid  decodeGuid(const QByteArray &bytes16);
QByteArray encodeGuid(const QUuid &uuid);

// Length-prefixed UTF-16LE string: u32 byte-length, then that many bytes.
QString    decodeUnicodeString(const QByteArray &data, int offset, int *bytesConsumed = nullptr);
QByteArray encodeUnicodeString(const QString &s);

// Little-endian primitive readers (bounds-safe: return false on short input).
bool readU16(const QByteArray &d, int off, quint16 *out);
bool readU32(const QByteArray &d, int off, quint32 *out);
bool readI32(const QByteArray &d, int off, qint32 *out);

} // namespace FieldDecoders

#endif // FIELDDECODERS_H
