// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic: dump an entity field map from "   114/Props" with MPXJ semantics.
// usage: dump_props <file.mpp> [task|resource|assignment]

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
        if (idx <= 30 || idx == 300)
            std::printf("idx=%-4u type=0x%08x loc=%-5s offset=%-5u block=%d\n",
                        idx, typeValue, loc, dataBlockOffset, blockIdx);
    }
    return 0;
}
