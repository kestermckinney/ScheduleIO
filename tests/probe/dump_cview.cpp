// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic (throwaway): validate the "   214/CV_iew" VarMeta/Var2Data pair
// for structural corruption - out-of-bounds blob offsets, header dataSize
// mismatch, and dump the Gantt view's Props9 item list.
// usage: dump_cview <file.mpp>

#include "ole/compoundfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QPair>
#include <QString>
#include <QVector>
#include <QtEndian>
#include <cstdio>

static quint32 u32(const QByteArray &d, int o)
{ return (o < 0 || o + 4 > d.size()) ? 0 : qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(d.constData()) + o); }
static quint16 u16(const QByteArray &d, int o)
{ return (o < 0 || o + 2 > d.size()) ? 0 : qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(d.constData()) + o); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: dump_cview <file.mpp>\n"); return 2; }

    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open\n"); return 2; }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) { std::printf("CFB parse failed\n"); return 2; }

    const QStringList base = { QStringLiteral("   214"), QStringLiteral("CV_iew") };
    const QByteArray fixedMeta = cf.readStream(base + QStringList{ QStringLiteral("FixedMeta") });
    const QByteArray fixedData = cf.readStream(base + QStringList{ QStringLiteral("FixedData") });
    const QByteArray varMeta = cf.readStream(base + QStringList{ QStringLiteral("VarMeta") });
    const QByteArray var2 = cf.readStream(base + QStringList{ QStringLiteral("Var2Data") });

    std::printf("FixedMeta=%d FixedData=%d VarMeta=%d Var2Data=%d\n",
                fixedMeta.size(), fixedData.size(), varMeta.size(), var2.size());

    if (varMeta.size() < 24 || u32(varMeta, 0) != 0xFADFADBAu) {
        std::printf("VarMeta: bad magic or too short!\n");
        return 1;
    }
    const int itemCount = int(u32(varMeta, 8));
    const int headerDataSize = int(u32(varMeta, 20));
    std::printf("VarMeta header: itemCount=%d headerDataSize=%d (actual Var2Data=%d) %s\n",
                itemCount, headerDataSize, var2.size(),
                headerDataSize == var2.size() ? "OK" : "*** MISMATCH ***");

    // Enumerate FixedData views: id@0, viewType@112, splitViewFlag@110.
    std::printf("\n-- Views (from FixedMeta/FixedData) --\n");
    const int fmItems = fixedMeta.size() >= 16 ? int(u32(fixedMeta, 8)) : 0;
    int lastOffset = -1;
    int ganttUid = -1;
    QVector<QPair<int, int>> views;   // (uid, viewType) for non-split views
    for (int i = 0; i < fmItems; ++i) {
        const int metaOff = 16 + i * 10;
        if (metaOff + 10 > fixedMeta.size()) break;
        const int offset = u16(fixedMeta, metaOff + 4);
        if (offset <= lastOffset) continue;
        lastOffset = offset;
        if (offset + 138 > fixedData.size()) continue;
        const quint32 id = u32(fixedData, offset + 0);
        const quint16 split = u16(fixedData, offset + 110);
        const quint16 vtype = u16(fixedData, offset + 112);
        // The UTF-16 view name starts at offset 4 in the record.
        QString name;
        for (int b = 4; b + 1 < 110; b += 2) {
            const ushort ch = u16(fixedData, offset + b);
            if (ch == 0) break;
            name.append(QChar(ch));
        }
        std::printf("  id=%u split=%u type=%u name=\"%s\"%s\n", id, split, vtype,
                    qPrintable(name), (split == 0 && vtype == 1) ? "  <- GANTT" : "");
        if (split == 0) {
            views.append({ int(id), int(vtype) });
            if (vtype == 1 && ganttUid < 0)
                ganttUid = int(id);
        }
    }
    std::printf("Resolved Gantt view uid = %d\n", ganttUid);

    // For every non-split view, find its PROPERTIES (type=6) var record and dump
    // the Props9 item keys, flagging STYLE_DATA (574619656) and dumping its first
    // text-style slots (32-byte stride @ 26) to see if the Gantt layout carries
    // over to Resource Usage / Calendar / Task Usage views.
    std::printf("\n-- Per-view Props9 (STYLE_DATA probe) --\n");
    for (const QPair<int, int> &v : views) {
        int propsOff = -1, propsLen = -1;
        for (int i = 0; i < itemCount; ++i) {
            const int o = 24 + i * 12;
            if (o + 12 > varMeta.size()) break;
            if (int(u32(varMeta, o)) == v.first && u16(varMeta, o + 8) == 6) {
                propsOff = int(u32(varMeta, o + 4));
                propsLen = int(u32(var2, propsOff));
                break;
            }
        }
        std::printf("view uid=%d type=%d: ", v.first, v.second);
        if (propsOff < 0) { std::printf("no PROPERTIES record\n"); continue; }
        const QByteArray props = var2.mid(propsOff + 4, propsLen);
        if (props.size() < 16) { std::printf("props too short (%d)\n", props.size()); continue; }
        const int pc = u16(props, 12);
        std::printf("propsLen=%d items=%d keys=[", propsLen, pc);
        int po = 16;
        int styleOff = -1, styleSize = 0;
        for (int i = 0; i < pc; ++i) {
            if (po + 12 > props.size()) break;
            const quint32 size = u32(props, po);
            const quint32 key = u32(props, po + 4);
            po += 12;
            std::printf("%u(%u) ", key, size);
            if (key == 574619656u) { styleOff = po; styleSize = int(size); }
            po += int(size);
            if (size % 2 != 0) ++po;
        }
        std::printf("]\n");
        if (styleOff >= 0) {
            std::printf("  STYLE_DATA size=%d; first text slots @26+32n:\n", styleSize);
            for (int c = 0; c < 12; ++c) {
                const int so = styleOff + 26 + c * 32;
                if (so + 32 > props.size() || so + 32 > styleOff + styleSize) break;
                const int bits = uchar(props.at(so + 3));
                auto col = [&](int co) -> QString {
                    if (uchar(props.at(so + co + 3)) != 0) return QStringLiteral("auto");
                    return QStringLiteral("%1,%2,%3").arg(uchar(props.at(so+co)))
                        .arg(uchar(props.at(so+co+1))).arg(uchar(props.at(so+co+2)));
                };
                std::printf("    [%2d] fontIdx=%u bits=0x%02x color=%-11s back=%-11s pat=%u\n",
                            c, uchar(props.at(so)), bits, qPrintable(col(4)),
                            qPrintable(col(16)), u16(props, so + 28));
            }
        }
    }

    // Walk every VarMeta record, bounds-check its blob against Var2Data.
    std::printf("\n-- VarMeta records (bounds check) --\n");
    int bad = 0;
    int ganttPropsOffset = -1, ganttPropsLen = -1;
    for (int i = 0; i < itemCount; ++i) {
        const int o = 24 + i * 12;
        if (o + 12 > varMeta.size()) { std::printf("  record %d: truncated VarMeta!\n", i); ++bad; break; }
        const quint32 uid = u32(varMeta, o);
        const quint32 off = u32(varMeta, o + 4);
        const quint16 type = u16(varMeta, o + 8);
        const quint16 typeHigh = u16(varMeta, o + 10);
        const quint32 blobLen = u32(var2, int(off));
        const bool ok = (int(off) + 4 + int(blobLen) <= var2.size()) && off + 4 <= quint32(var2.size());
        std::printf("  rec %d: uid=%u type=%u typeHigh=%u offset=%u len=%u\n", i, uid, type, typeHigh, off, blobLen);
        if (!ok) {
            std::printf("  *** BAD *** uid=%u type=%u offset=%u declaredLen=%u (Var2Data size=%d)\n",
                        uid, type, off, blobLen, var2.size());
            ++bad;
        }
        if (ganttUid >= 0 && uid == quint32(ganttUid) && type == 6) {
            ganttPropsOffset = int(off);
            ganttPropsLen = int(blobLen);
        }
    }
    std::printf("Total records=%d, bad=%d\n", itemCount, bad);

    if (ganttPropsOffset < 0) {
        std::printf("\nNo PROPERTIES (type=6) var record found for Gantt view uid=%d\n", ganttUid);
        return bad ? 1 : 0;
    }

    std::printf("\n-- Gantt view Props9 (uid=%d, offset=%d, len=%d) --\n", ganttUid, ganttPropsOffset, ganttPropsLen);
    std::printf("Props9 16-byte header:");
    for (int b = 0; b < 16 && ganttPropsOffset + 4 + b < var2.size(); ++b)
        std::printf(" %02x", uchar(var2.at(ganttPropsOffset + 4 + b)));
    std::printf("\n");
    const QByteArray props = var2.mid(ganttPropsOffset + 4, ganttPropsLen);
    if (props.size() < 16) { std::printf("Props9 block too short\n"); return 1; }
    const int propItemCount = u16(props, 12);
    std::printf("Props9 item count=%d (block size=%d)\n", propItemCount, props.size());
    int po = 16;
    for (int i = 0; i < propItemCount; ++i) {
        if (po + 12 > props.size()) { std::printf("  item %d: truncated!\n", i); break; }
        const quint32 size = u32(props, po);
        const quint32 key = u32(props, po + 4);
        const quint32 flags = u32(props, po + 8);
        po += 12;
        const bool ok = (po + int(size) <= props.size());
        const quint16 crc = ok ? qChecksum(props.constData() + po, int(size)) : 0;
        std::printf("  item %d: key=%u size=%u flags=0x%x crc16=0x%04x %s\n", i, key, size, flags, crc, ok ? "" : "*** OUT OF BOUNDS ***");
        if (!ok) break;
        if (key == 574619656u) {
            std::printf("    -> STYLE_DATA: barCount@2243=%d (blob size=%u)\n",
                        size > 2243 ? uchar(props.at(po + 2243)) : -1, size);
            static const char *kCatNames[] = {
                "Highlighted", "RowAndColumn", "NonCritical", "Critical", "Summary",
                "Milestone", "MiddleTimescale", "BottomTimescale", "BarTextLeft",
                "BarTextRight", "BarTextTop", "BarTextBottom", "BarTextInside",
                "Marked", "ProjectSummary", "External", "TopTimescale"
            };
            for (int c = 0; c < 17; ++c) {
                const int so = po + 26 + c * 32;
                if (so + 32 > props.size()) break;
                const int bits = uchar(props.at(so + 3));
                auto colorAt = [&](int co) -> QString {
                    if (uchar(props.at(so + co + 3)) != 0) return QStringLiteral("auto");
                    return QStringLiteral("%1,%2,%3")
                        .arg(uchar(props.at(so+co))).arg(uchar(props.at(so+co+1))).arg(uchar(props.at(so+co+2)));
                };
                const quint16 backPattern = u16(props, so + 28);
                std::printf("      [%2d] %-16s bold=%d color=%-12s backColor=%-12s backPattern=%u\n",
                            c, kCatNames[c], bits & 1,
                            qPrintable(colorAt(4)), qPrintable(colorAt(16)), backPattern);
            }
        }
        if (key == 574619660u) {
            std::printf("    -> COLUMN_PROPERTIES: %u bytes = %.2f records (should be integer!)\n",
                        size, size / 44.0);
            for (quint32 r = 0; r + 44 <= size; r += 44) {
                std::printf("    record %u:\n", r / 44);
                for (quint32 b = 0; b < 44; ++b)
                    std::printf("      [%2u] = 0x%02x\n", b, uchar(props.at(po + int(r) + int(b))));
            }
        }
        po += int(size);
        if (size % 2 != 0) ++po;
    }

    return bad ? 1 : 0;
}
