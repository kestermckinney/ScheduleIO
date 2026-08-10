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
            std::printf("  uid=%-6u  o8=%.2f o16=%.2f o24=%.2f o32=%.2f o40=%.2f o48=%.2f o56=%.2f priority@78=%u durationUnits@164=0x%04x\n",
                        u32(b, 4), dbl(b,8), dbl(b,16), dbl(b,24), dbl(b,32), dbl(b,40), dbl(b,48), dbl(b,56),
                        b.size() >= 80 ? u16(b, 78) : 0, b.size() >= 166 ? u16(b, 164) : 0);
            ++shown;
        }
    }

    // Dump native cost-rate tables A-E (keys 61-65), availability (276), and
    // fixed-meta records for resource-layout interoperability work.
    if (which == "resource") {
        BkndVarData v;
        v.parse(cf.readStream({ QStringLiteral("   114"), sub, QStringLiteral("VarMeta") }),
                cf.readStream({ QStringLiteral("   114"), sub, QStringLiteral("Var2Data") }));
        const QByteArray fixedMeta = cf.readStream(
            { QStringLiteral("   114"), sub, QStringLiteral("FixedMeta") });
        const int metaRecordSize = 37;
        for (int o = 16; o + metaRecordSize <= fixedMeta.size(); o += metaRecordSize) {
            std::printf("--- resource fixed-meta record=%d ---\n", (o - 16) / metaRecordSize);
            for (int i = 0; i < metaRecordSize; ++i)
                std::printf("%02x%c", static_cast<unsigned char>(fixedMeta.at(o + i)),
                            i + 1 == metaRecordSize ? '\n' : ' ');
        }
        for (const auto &e : v.stringsForType(1)) {   // iterate resource uids via NAME
            for (quint16 key = 61; key <= 65; ++key) {
                const QByteArray rateBlob = v.blobFor(e.uniqueId, key);
                if (rateBlob.isEmpty())
                    continue;
                std::printf("--- cost-rate blob uid=%u key=%u len=%d ---\n",
                            e.uniqueId, key, rateBlob.size());
                for (int i = 0; i < rateBlob.size(); ++i) {
                    std::printf("%02x ", static_cast<unsigned char>(rateBlob.at(i)));
                    if (i % 44 == 43) std::printf("\n");
                }
                std::printf("\n");
            }
            const QByteArray blob = v.blobFor(e.uniqueId, 276);
            if (blob.isEmpty())
                continue;
            std::printf("--- availability blob uid=%u len=%d ---\n", e.uniqueId, blob.size());
            for (int i = 0; i < blob.size(); ++i) {
                std::printf("%02x ", static_cast<unsigned char>(blob.at(i)));
                if (i % 20 == 19) std::printf("\n");
            }
            std::printf("\n");
        }
    }

    // Raw timephased assignment blobs: remaining regular work (49) and actual
    // regular work (50). These are useful when pinning the cumulative-work and
    // elapsed-time record layouts against a paired MSPDI export.
    if (which == "assignment") {
        const QByteArray fixedMeta = cf.readStream(
            { QStringLiteral("   114"), sub, QStringLiteral("FixedMeta") });
        const QByteArray fixedData = cf.readStream(
            { QStringLiteral("   114"), sub, QStringLiteral("FixedData") });
        auto dbl = [](const QByteArray &b, int o) {
            if (o + 8 > b.size()) return 0.0;
            quint64 bits = qFromLittleEndian<quint64>(
                reinterpret_cast<const uchar *>(b.constData()) + o);
            double value; memcpy(&value, &bits, 8); return value;
        };
        const int count = qMax(0, (fixedMeta.size() - 16) / 34);
        if (fixedMeta.size() >= 50) {
            std::printf("assignment-meta: ");
            for (int i = 16; i < 50; ++i)
                std::printf("%02x ", static_cast<unsigned char>(fixedMeta.at(i)));
            std::printf("\n");
        }
        std::printf("--- assignment block-0 work doubles ---\n");
        for (int loop = 0; loop < count; ++loop) {
            const quint32 offset = u32(fixedMeta, 16 + loop * 34 + 4);
            if (offset + 52 > quint32(fixedData.size())) continue;
            const QByteArray record = fixedData.mid(int(offset), 110);
            std::printf("uid=%u work=%.0f actual=%.0f regular=%.0f remaining=%.0f\n",
                        u32(record, 0), dbl(record, 20), dbl(record, 28),
                        dbl(record, 36), dbl(record, 44));
        }
        BkndVarData v;
        v.parse(cf.readStream({ QStringLiteral("   114"), sub, QStringLiteral("VarMeta") }),
                cf.readStream({ QStringLiteral("   114"), sub, QStringLiteral("Var2Data") }));
        for (quint16 key : { quint16(9), quint16(13), quint16(14),
                             quint16(49), quint16(50), quint16(51) }) {
            std::printf("--- assignment timephased key=%u ---\n", key);
            for (quint32 uid = 0; uid < 100000; ++uid) {
                const QByteArray blob = v.blobFor(uid, key);
                if (blob.isEmpty())
                    continue;
                std::printf("uid=%u len=%d: ", uid, blob.size());
                for (int i = 0; i < blob.size(); ++i)
                    std::printf("%02x ", static_cast<unsigned char>(blob.at(i)));
                std::printf("\n");
            }
        }
    }

    // Var-data type histogram for the entity (which var keys actually carry data).
    const QByteArray vm = cf.readStream({ QStringLiteral("   114"), sub, QStringLiteral("VarMeta") });
    QMap<quint16, int> hist;
    for (int o = 24; o + 12 <= vm.size(); o += 12) {
        hist[u16(vm, o + 8)]++;
        if (which == "assignment"
            && (u16(vm, o + 8) == 9 || u16(vm, o + 8) == 13
                || u16(vm, o + 8) == 14 || u16(vm, o + 8) == 51))
            std::printf("selected-var uid=%u type=%u high=0x%04x offset=%u\n",
                        u32(vm, o), u16(vm, o + 8), u16(vm, o + 10),
                        u32(vm, o + 4));
    }
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
        std::printf("--- physical-progress var blobs ---\n");
        shown = 0;
        for (const auto &e : v.stringsForType(14)) {
            const QByteArray physical = v.blobFor(e.uniqueId, 1119);
            const QByteArray method = v.blobFor(e.uniqueId, 1122);
            if (physical.isEmpty() && method.isEmpty())
                continue;
            std::printf("  uid=%-6u physical[len=%d]=%s method[len=%d]=%s\n",
                        e.uniqueId, physical.size(), physical.toHex(' ').constData(),
                        method.size(), method.toHex(' ').constData());
            if (++shown >= 8) break;
        }
    }
    return 0;
}
