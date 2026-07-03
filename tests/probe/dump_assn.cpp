// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic: decode candidate assignment scheduling fields (start/finish/
// actual work/delay) straight from TBkndAssn FixedData so their field-map
// indices can be pinned against the MS Project XML export of the same file.
// usage: dump_assn <file.mpp>

#include "ole/compoundfile.h"
#include "codec/fieldmap.h"
#include "codec/fielddecoders.h"
#include "codec/propsreader.h"

#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <cstdio>
#include <cstring>

static double dbl(const QByteArray &b, int o)
{
    if (o < 0 || o + 8 > b.size()) return -1;
    quint64 bits = qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(b.constData()) + o);
    double v; std::memcpy(&v, &bits, 8); return v;
}
static qint32 i32(const QByteArray &b, int o)
{
    if (o < 0 || o + 4 > b.size()) return -1;
    return qFromLittleEndian<qint32>(reinterpret_cast<const uchar *>(b.constData()) + o);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: dump_assn <file.mpp>\n"); return 2; }
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open\n"); return 2; }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) { std::printf("cfb parse failed\n"); return 2; }

    PropsReader props;
    props.parse(cf.readStream({ QStringLiteral("   114"), QStringLiteral("Props") }));
    const QByteArray fm = FieldMap::fieldMapBytes(props, 0x00020017u, 0x03000017u);
    const QHash<quint16, int> off = FieldMap::block0FixedOffsets(fm, 0x0F40);

    const QStringList idxDump = { "10", "11", "12", "20", "21", "24", "25", "55", "264" };
    for (const QString &s : idxDump)
        std::printf("idx %-4s -> offset %d\n", qPrintable(s), off.value(quint16(s.toUInt()), -1));

    const QByteArray meta = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndAssn"), QStringLiteral("FixedMeta") });
    const QByteArray data = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndAssn"), QStringLiteral("FixedData") });
    const int count = (meta.size() - 16) / 34;
    int shown = 0;
    for (int loop = 0; loop < count && shown < 200; ++loop) {
        const int metaPos = 16 + loop * 34;
        if (meta.at(metaPos) != 0) continue;   // dead row
        const quint32 recOff = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(meta.constData()) + metaPos + 4);
        const quint32 next = (loop + 1 < count)
            ? qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(meta.constData()) + metaPos + 34 + 4)
            : quint32(data.size());
        if (recOff >= quint32(data.size()) || next <= recOff) continue;
        const QByteArray b = data.mid(int(recOff), int(next - recOff));

        const int uid = i32(b, off.value(0, -1));
        const int task = i32(b, off.value(1, -1));
        const int res = i32(b, off.value(2, -1));
        auto date = [&](quint16 idx) {
            const int o = off.value(idx, -1);
            if (o < 0) return QString("--");
            const QDateTime dt = FieldDecoders::decodeMppTimestamp(b, o);
            return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd hh:mm")) : QString("invalid");
        };
        auto work = [&](quint16 idx) {
            const int o = off.value(idx, -1);
            return o < 0 ? -1.0 : double(FieldDecoders::decodeWorkDouble(dbl(b, o))) / 3600000.0;   // hours
        };
        std::printf("uid=%-4d task=%-4d res=%-4d | units(raw@12)=%g | d20=%s d21=%s d24=%s d264=%s | w8=%.2fh w10=%.2fh w11=%.2fh w12=%.2fh | i25=%d i55(u16 raw)=%d\n",
                    uid, task, res, dbl(b, off.value(7, -1)),
                    qPrintable(date(20)), qPrintable(date(21)), qPrintable(date(24)), qPrintable(date(264)),
                    work(8), work(10), work(11), work(12),
                    i32(b, off.value(25, -1)),
                    off.value(55, -1) >= 0 && off.value(55, -1) + 2 <= b.size()
                        ? qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(b.constData()) + off.value(55, -1)) : -1);
        ++shown;
    }
    return 0;
}
