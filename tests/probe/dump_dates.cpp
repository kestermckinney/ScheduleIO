// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Throwaway diagnostic: reads task Start/Finish/Duration from TBkndTask FixedData
// using MPXJ's FixedMeta/FixedData model and timestamp codec, to validate offsets
// against the XML oracle before porting into the library.

#include "mppio.h"
#include "ole/compoundfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QtEndian>
#include <cstdio>

static quint32 i32(const QByteArray &d, int o)
{ return (o + 4 <= d.size()) ? qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(d.constData()) + o) : 0; }
static quint16 i16(const QByteArray &d, int o)
{ return (o + 2 <= d.size()) ? qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(d.constData()) + o) : 0; }

// MPXJ MPPUtility.getTimestamp: days @ offset+2, time(tenths-min) @ offset; epoch 1983-12-31.
static QDateTime ts(const QByteArray &d, int offset)
{
    quint16 days = i16(d, offset + 2);
    if (days <= 1 || days == 65535) return QDateTime();
    quint16 time = i16(d, offset);
    if (time == 65535) time = 0;
    return QDateTime(QDate(1983, 12, 31), QTime(0, 0), Qt::UTC)
        .addDays(days).addSecs(qint64(time) * 6);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: dump_dates <file.mpp>\n"); return 2; }
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open\n"); return 2; }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) { std::printf("parse failed\n"); return 2; }

    auto rd = [&](const char *s) {
        return cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndTask"), QString::fromLatin1(s) });
    };
    const QByteArray meta = rd("FixedMeta");
    const QByteArray data = rd("FixedData");
    const QByteArray meta2 = rd("Fixed2Meta");
    const QByteArray data2 = rd("Fixed2Data");
    const int count = (meta.size() - 16) / 47;          // 16-byte header, 47-byte items
    // Fixed2Meta item size differs; derive it from its own count == count.
    const int item2 = (count > 0) ? (meta2.size() - 16) / count : 0;
    std::printf("FixedMeta items=%d FixedData=%d  Fixed2Meta item=%d Fixed2Data=%d\n",
                count, data.size(), item2, data2.size());
    auto block2For = [&](int loop) -> QByteArray {
        if (item2 <= 0) return QByteArray();
        quint32 o = i32(meta2, 16 + loop * item2 + 4);
        quint32 nx = quint32(data2.size());
        if (loop + 1 < count) nx = i32(meta2, 16 + (loop + 1) * item2 + 4);
        if (o >= quint32(data2.size()) || nx <= o) return QByteArray();
        return data2.mid(int(o), int(nx - o));
    };

    // Offsets discovered from the task field map (block 0): UID@4, DUR@84, START@96, FINISH@100.
    int printed = 0;
    for (int loop = 0; loop < count; ++loop) {
        const int metaPos = 16 + loop * 47;
        const quint32 itemOffset = i32(meta, metaPos + 4);
        quint32 nextOffset = quint32(data.size());
        if (loop + 1 < count) nextOffset = i32(meta, 16 + (loop + 1) * 47 + 4);
        if (itemOffset >= quint32(data.size()) || nextOffset <= itemOffset) continue;
        const QByteArray block = data.mid(int(itemOffset), int(nextOffset - itemOffset));
        if (block.size() < 104) continue;

        const quint32 uid = i32(block, 4);
        const QByteArray b2 = block2For(loop);
        const QDateTime s0 = ts(block, 96);
        const QDateTime s1 = ts(b2, 50);     // field 1283 "Task Start" in Fixed2Data
        (void)printed;
        const quint16 pc = i16(block, 92);   // PERCENT_COMPLETE field 32
        std::printf("uid=%-5u pc92=%u\n", uid, pc);
        (void)s0; (void)s1;
    }
    return 0;
}
