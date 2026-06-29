// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic: dump the FULL entity field map from "   114/Props" plus a var-data
// type histogram, to ground cost/baseline/custom field indices during RE work.
// usage: dump_fields <file.mpp> [task|resource|assignment]

#include "ole/compoundfile.h"
#include "codec/bkndvardata.h"
#include "codec/fielddecoders.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QMap>
#include <QtEndian>
#include <cstdio>

static quint32 u32(const QByteArray &d, int o)
{ return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(d.constData()) + o); }
static quint16 u16(const QByteArray &d, int o)
{ return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(d.constData()) + o); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: dump_fields <file.mpp> [task|resource|assignment]\n"); return 2; }
    const QByteArray which = (argc >= 3) ? QByteArray(argv[2]) : QByteArray("task");

    quint32 key1 = 0x00020014u, key2 = 0x03000014u; QString sub = "TBkndTask"; quint16 hi = 0x0B40;
    if (which == "resource")   { key1 = 0x00020015u; key2 = 0x03000015u; sub = "TBkndRsc";  hi = 0x0C40; }
    else if (which == "assignment") { key1 = 0x00020017u; key2 = 0x03000017u; sub = "TBkndAssn"; hi = 0x0F40; }

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
    std::printf("=== %s field map: %d bytes (%d entries), highWord=0x%04x ===\n",
                which.constData(), map.size(), map.size() / 28, hi);

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
        if ((typeValue >> 16) != hi) continue;   // only this entity's fields
        std::printf("idx=%-5u type=0x%08x loc=%-5s offset=%-5u block=%d category=0x%02x\n",
                    idx, typeValue, loc, dataBlockOffset, blockIdx, category);
    }

    // Per-task value probe: print block-0 doubles for a few tasks so cost/baseline
    // scaling can be checked against the XML.
    if (which == "task") {
        const QByteArray meta = cf.readStream({ QStringLiteral("   114"), sub, QStringLiteral("FixedMeta") });
        const QByteArray data = cf.readStream({ QStringLiteral("   114"), sub, QStringLiteral("FixedData") });
        const int count = (meta.size() - 16) / 47;
        auto dbl = [&](const QByteArray &b, int o) {
            if (o + 8 > b.size()) return 0.0;
            quint64 bits = qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(b.constData()) + o);
            double v; memcpy(&v, &bits, 8); return v; };
        std::printf("--- task block-0 doubles (uid @4; off 8/16/24/32/40/48/56) ---\n");
        int shown = 0;
        for (int loop = 0; loop < count && shown < 14; ++loop) {
            quint32 off = u32(meta, 16 + loop * 47 + 4);
            quint32 nx = (loop + 1 < count) ? u32(meta, 16 + (loop + 1) * 47 + 4) : quint32(data.size());
            if (off >= quint32(data.size()) || nx <= off) continue;
            const QByteArray b = data.mid(int(off), int(nx - off));
            if (b.size() < 64) continue;
            std::printf("  uid=%-6u  o8=%.2f o16=%.2f o24=%.2f o32=%.2f o40=%.2f o48=%.2f o56=%.2f\n",
                        u32(b, 4), dbl(b,8), dbl(b,16), dbl(b,24), dbl(b,32), dbl(b,40), dbl(b,48), dbl(b,56));
            ++shown;
        }
    }

    // Var-data type histogram for the entity (which var keys actually carry data).
    const QByteArray vm = cf.readStream({ QStringLiteral("   114"), sub, QStringLiteral("VarMeta") });
    QMap<quint16, int> hist;
    for (int o = 24; o + 12 <= vm.size(); o += 12)
        hist[u16(vm, o + 8)]++;
    std::printf("--- %s var-data types present (type:count) ---\n", which.constData());
    for (auto it = hist.constBegin(); it != hist.constEnd(); ++it)
        std::printf("  type=%-5u count=%d\n", it.key(), it.value());

    // Dump baseline-0 var blobs (work=1, cost=6, dur=27, start=43, finish=44) for a
    // few task uids that actually have them, to inspect the encoding.
    if (which == "task") {
        BkndVarData v;
        v.parse(vm, cf.readStream({ QStringLiteral("   114"), sub, QStringLiteral("Var2Data") }));
        std::printf("--- baseline-0 var blobs per uid ---\n");
        int shown = 0;
        for (const auto &e : v.stringsForType(14)) {   // iterate task uids via NAME
            const quint32 uid = e.uniqueId;
            const QByteArray work = v.blobFor(uid, 1), cost = v.blobFor(uid, 6);
            if (work.isEmpty() && cost.isEmpty())
                continue;
            double wd = 0, cd = 0;
            FieldDecoders::readDouble(work, 0, &wd);
            FieldDecoders::readDouble(cost, 0, &cd);
            std::printf("  uid=%-6u work[len=%d]=%.3f  cost[len=%d]=%.3f\n",
                        uid, work.size(), wd, cost.size(), cd);
            if (++shown >= 8) break;
        }
    }
    return 0;
}
