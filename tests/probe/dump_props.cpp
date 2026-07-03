// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic: dump an entity field map from "   114/Props" with MPXJ semantics.
// usage: dump_props <file.mpp> [task|resource|assignment]

#include "codec/bkndvardata.h"
#include "ole/compoundfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <cstdio>

static quint32 u32(const QByteArray &d, int o)
{ return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(d.constData()) + o); }
static quint16 u16(const QByteArray &d, int o)
{ return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(d.constData()) + o); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: dump_props <file.mpp> [task|resource|assignment]\n"); return 2; }
    const QByteArray which = (argc >= 3) ? QByteArray(argv[2]) : QByteArray("task");

    quint32 key1 = 0x00020014u, key2 = 0x03000014u;   // task
    if (which == "resource") { key1 = 0x00020015u; key2 = 0x03000015u; }
    else if (which == "assignment") { key1 = 0x00020017u; key2 = 0x03000017u; }

    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open\n"); return 2; }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) { std::printf("parse failed\n"); return 2; }
    const QByteArray props = cf.readStream({ QStringLiteral("   114"), QStringLiteral("Props") });
    if (props.size() < 16) { std::printf("no   114/Props\n"); return 1; }

    if (which == "cal") {
        const QByteArray meta = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndCal"), QStringLiteral("FixedMeta") });
        const QByteArray data = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndCal"), QStringLiteral("FixedData") });
        std::printf("TBkndCal FixedMeta=%d FixedData=%d  count(meta/10)=%d\n",
                    meta.size(), data.size(), (meta.size() - 16) / 10);
        const int count = (meta.size() - 16) / 10;
        int printed = 0;
        for (int loop = 0; loop < count; ++loop) {
            quint32 off = u32(meta, 16 + loop * 10 + 4);
            quint32 nx = (loop + 1 < count) ? u32(meta, 16 + (loop + 1) * 10 + 4) : quint32(data.size());
            if (off >= quint32(data.size()) || nx <= off) continue;
            const QByteArray b = data.mid(int(off), int(nx - off));
            if (printed++ < 14)
                std::printf("  loop=%d off=%u size=%d  base@0=%u res@4=%u cal@8=%u\n",
                            loop, off, b.size(),
                            b.size() >= 4 ? u32(b, 0) : 0, b.size() >= 8 ? u32(b, 4) : 0,
                            b.size() >= 12 ? u32(b, 8) : 0);
        }
        // List the calendar var records (uid, type, blob len) directly.
        const QByteArray vm = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndCal"), QStringLiteral("VarMeta") });
        const QByteArray v2 = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndCal"), QStringLiteral("Var2Data") });
        std::printf("cal VarMeta=%d Var2Data=%d\n", vm.size(), v2.size());
        int shown = 0;
        for (int o = 24; o + 12 <= vm.size() && shown < 24; o += 12) {
            const quint32 uid = u32(vm, o), voff = u32(vm, o + 4);
            const quint16 type = u16(vm, o + 8);
            const quint32 blen = (int(voff) + 4 <= v2.size()) ? u32(v2, int(voff)) : 0;
            std::printf("  uid=%u type=%u off=%u len=%u\n", uid, type, voff, blen);
            ++shown;
        }
        return 0;
    }

    if (which == "rawprops") {
        for (int o = 16; o + 12 <= props.size(); ) {
            const quint32 len = u32(props, o), key = u32(props, o + 4), flags = u32(props, o + 8);
            o += 12;
            if (len > quint32(props.size() - o)) break;
            std::printf("key=%u (0x%08x) len=%u flags=0x%08x\n", key, key, len, flags);
            o += int(len);
        }
        return 0;
    }

    QByteArray map1, map2;
    for (int o = 16; o + 12 <= props.size(); ) {
        const quint32 len = u32(props, o), key = u32(props, o + 4);
        o += 12;
        if (len > quint32(props.size() - o)) break;
        if (key == key1) map1 = props.mid(o, int(len));
        if (key == key2) map2 = props.mid(o, int(len));
        o += int(len);
    }
    const QByteArray map = !map1.isEmpty() ? map1 : map2;
    std::printf("%s field map: %d bytes (%d entries)\n", which.constData(), map.size(), map.size() / 28);

    if (which == "resource" && argc < 4) {
        const QByteArray meta = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndRsc"), QStringLiteral("FixedMeta") });
        const QByteArray data = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndRsc"), QStringLiteral("FixedData") });
        const int count = (meta.size() - 16) / 37;
        auto dbl = [&](const QByteArray &b, int o) {
            if (o + 8 > b.size()) return 0.0;
            quint64 bits = qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(b.constData()) + o);
            double v; memcpy(&v, &bits, 8); return v; };
        int shown = 0;
        for (int loop = 0; loop < count && shown < 12; ++loop) {
            quint32 off = u32(meta, 16 + loop * 37 + 4);
            quint32 nx = (loop + 1 < count) ? u32(meta, 16 + (loop + 1) * 37 + 4) : quint32(data.size());
            if (off >= quint32(data.size()) || nx <= off) continue;
            const QByteArray b = data.mid(int(off), int(nx - off));
            if (b.size() < 24) continue;
            std::printf("  uid@4=%u  d@8=%.4f  i@16=%u  d@16=%.4f  d@24=%.4f\n",
                        u32(b, 4), dbl(b, 8), u32(b, 16), dbl(b, 16), dbl(b, 24));
            ++shown;
        }
        return 0;
    }

    int lastOffset = 0, blockIndex = 0;
    for (int i = 0; i + 28 <= map.size(); i += 28) {
        const quint32 typeValue = u32(map, i + 12);
        const quint16 dataBlockOffset = u16(map, i + 4);
        const quint16 category = u16(map, i + 20);
        const quint32 idx = typeValue & 0xFFFFu;
        const char *loc = "VAR";
        int blockIdx = 0;
        if (category == 0x0B || category == 0x64) loc = "META";
        else if (dataBlockOffset != 0xFFFF) {
            loc = "FIXED";
            if (dataBlockOffset < lastOffset) ++blockIndex;
            lastOffset = dataBlockOffset;
            blockIdx = blockIndex;
        }
        // Print the low-index fields (the core ones) to keep output readable.
        if (idx <= 40 || idx == 299 || idx == 300)
            std::printf("idx=%-4u type=0x%08x loc=%-5s offset=%-5u block=%d\n",
                        idx, typeValue, loc, dataBlockOffset, blockIdx);
    }
    return 0;
}
