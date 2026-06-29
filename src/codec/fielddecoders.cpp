// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/fielddecoders.h"

#include <QtEndian>

#include <cmath>
#include <cstring>

namespace FieldDecoders {

QDateTime epoch()
{
    // MS Project counts time from 1984-01-01. Use UTC for cross-platform stability.
    return QDateTime(QDate(1984, 1, 1), QTime(0, 0, 0), Qt::UTC);
}

// Sentinel for "no date" (an invalid/null QDateTime), mirroring how MS Project
// marks unset date fields rather than defaulting them to the epoch.
static constexpr quint32 kNoDate = 0xFFFFFFFF;

QDateTime decodeTimestampSeconds(quint32 seconds)
{
    if (seconds == kNoDate)
        return QDateTime();   // invalid == "no date"
    return epoch().addSecs(static_cast<qint64>(seconds));
}

quint32 encodeTimestampSeconds(const QDateTime &dt)
{
    if (!dt.isValid())
        return kNoDate;
    const qint64 secs = epoch().secsTo(dt.toUTC());
    if (secs < 0)
        return 0;
    return static_cast<quint32>(secs);
}

QDateTime decodeMppTimestamp(const QByteArray &block, int offset)
{
    quint16 days = 0, time = 0;
    if (!readU16(block, offset + 2, &days))
        return QDateTime();
    if (days <= 1 || days == 65535)
        return QDateTime();             // "no date" sentinels
    readU16(block, offset, &time);
    if (time == 65535)
        time = 0;
    return QDateTime(QDate(1983, 12, 31), QTime(0, 0), Qt::UTC)
        .addDays(days)
        .addSecs(static_cast<qint64>(time) * 6);   // time is tenths of a minute
}

QDateTime decodeTimestampTenths(const QByteArray &d, int offset)
{
    qint32 tenths = 0;
    if (!readI32(d, offset, &tenths))
        return QDateTime();
    return epoch().addSecs(static_cast<qint64>(tenths) * 6);   // 1 tenth-minute == 6 s
}

qint64 decodeDurationTenthMinutes(qint32 raw)
{
    // raw is in tenths of a minute -> milliseconds.
    return static_cast<qint64>(raw) * 6000;
}

qint32 encodeDurationTenthMinutes(qint64 millis)
{
    return static_cast<qint32>(millis / 6000);
}

bool readDouble(const QByteArray &d, int off, double *out)
{
    if (off < 0 || off + 8 > d.size())
        return false;
    const quint64 bits = qFromLittleEndian<quint64>(
        reinterpret_cast<const uchar *>(d.constData() + off));
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    *out = v;
    return true;
}

qint64 decodeWorkDouble(double thousandthsOfMinute)
{
    // Work is stored in 1/1000 of a minute, so value * 60 == milliseconds
    // (e.g. 8h -> 480000 -> 28'800'000 ms). Verified against the XML oracle.
    return llround(thousandthsOfMinute * 60.0);
}

double decodePercent(quint16 raw)
{
    return static_cast<double>(raw) / 100.0;
}

quint16 encodePercent(double ratio)
{
    double pct = ratio * 100.0;
    if (pct < 0.0)
        pct = 0.0;
    if (pct > 100.0)
        pct = 100.0;
    return static_cast<quint16>(qRound(pct));
}

QUuid decodeGuid(const QByteArray &bytes16)
{
    if (bytes16.size() < 16)
        return QUuid();
    const uchar *p = reinterpret_cast<const uchar *>(bytes16.constData());
    const quint32 d1 = qFromLittleEndian<quint32>(p);
    const quint16 d2 = qFromLittleEndian<quint16>(p + 4);
    const quint16 d3 = qFromLittleEndian<quint16>(p + 6);
    return QUuid(d1, d2, d3,
                 p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
}

QByteArray encodeGuid(const QUuid &uuid)
{
    QByteArray out(16, '\0');
    uchar *p = reinterpret_cast<uchar *>(out.data());
    qToLittleEndian<quint32>(uuid.data1, p);
    qToLittleEndian<quint16>(uuid.data2, p + 4);
    qToLittleEndian<quint16>(uuid.data3, p + 6);
    for (int i = 0; i < 8; ++i)
        p[8 + i] = uuid.data4[i];
    return out;
}

QString decodeUnicodeString(const QByteArray &data, int offset, int *bytesConsumed)
{
    quint32 byteLen = 0;
    if (!readU32(data, offset, &byteLen)) {
        if (bytesConsumed)
            *bytesConsumed = 0;
        return QString();
    }
    const int strStart = offset + 4;
    if (byteLen == 0 || strStart + static_cast<int>(byteLen) > data.size()) {
        if (bytesConsumed)
            *bytesConsumed = 4;
        return QString();
    }
    const QString s = QString::fromUtf16(
        reinterpret_cast<const char16_t *>(data.constData() + strStart),
        static_cast<int>(byteLen / 2));
    if (bytesConsumed)
        *bytesConsumed = 4 + static_cast<int>(byteLen);
    return s;
}

QByteArray encodeUnicodeString(const QString &s)
{
    const QByteArray utf16(reinterpret_cast<const char *>(s.utf16()),
                           s.size() * 2);
    QByteArray out(4, '\0');
    qToLittleEndian<quint32>(static_cast<quint32>(utf16.size()),
                             reinterpret_cast<uchar *>(out.data()));
    out.append(utf16);
    return out;
}

bool readU16(const QByteArray &d, int off, quint16 *out)
{
    if (off < 0 || off + 2 > d.size())
        return false;
    *out = qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(d.constData() + off));
    return true;
}

bool readU32(const QByteArray &d, int off, quint32 *out)
{
    if (off < 0 || off + 4 > d.size())
        return false;
    *out = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(d.constData() + off));
    return true;
}

bool readI32(const QByteArray &d, int off, qint32 *out)
{
    quint32 u = 0;
    if (!readU32(d, off, &u))
        return false;
    *out = static_cast<qint32>(u);
    return true;
}

} // namespace FieldDecoders
