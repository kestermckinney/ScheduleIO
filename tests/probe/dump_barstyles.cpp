// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic: dump the Gantt Chart view's STYLE_DATA (Props9 key 574619656)
// default bar-style table field-by-field, plus raw hex of the first records and
// the bytes immediately after the array, so the 195-byte record layout and the
// opaque tail can be reverse-engineered against real MS Project files.
// usage: dump_barstyles <file.mpp> [recordsToHexDump]

#include "codec/barstylecodec.h"
#include "model/viewstyles.h"
#include "ole/compoundfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QtEndian>
#include <cstdio>

static quint32 u32(const QByteArray &d, int o)
{ return (o < 0 || o + 4 > d.size()) ? 0 : qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(d.constData()) + o); }
static quint16 u16(const QByteArray &d, int o)
{ return (o < 0 || o + 2 > d.size()) ? 0 : qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(d.constData()) + o); }
static quint64 u64(const QByteArray &d, int o)
{ return (o < 0 || o + 8 > d.size()) ? 0 : qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(d.constData()) + o); }
static quint8 u8(const QByteArray &d, int o)
{ return (o < 0 || o >= d.size()) ? 0 : quint8(d.at(o)); }

static QString colorAt(const QByteArray &d, int o)
{
    if (o + 4 > d.size()) return QStringLiteral("<oob>");
    if (u8(d, o + 3) != 0) return QStringLiteral("auto");
    return QStringLiteral("%1,%2,%3").arg(u8(d, o)).arg(u8(d, o + 1)).arg(u8(d, o + 2));
}

// Decode a bar-text / from-to field id: 0x0B40xxxx -> "task#xxxx", 0xFFFFFFFF -> "none".
static QString fieldId(quint32 v)
{
    if (v == 0xFFFFFFFFu || v == 0x0000FFFFu) return QStringLiteral("none");
    const quint32 hi = v >> 16, lo = v & 0xFFFF;
    if (hi == 0x0B40)
        return QStringLiteral("task#%1").arg(lo);
    return QStringLiteral("0x%1").arg(v, 8, 16, QChar('0'));
}

static void hexDump(const QByteArray &d, int off, int len, const char *label)
{
    std::printf("  %s (off=%d len=%d):\n", label, off, len);
    for (int i = 0; i < len; i += 16) {
        std::printf("    +%3d:", i);
        for (int j = 0; j < 16 && i + j < len; ++j)
            std::printf(" %02x", u8(d, off + i + j));
        std::printf("\n");
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: dump_barstyles <file.mpp> [recordsToHexDump]\n"); return 2; }
    const int hexRecords = argc >= 3 ? QString::fromLocal8Bit(argv[2]).toInt() : 3;

    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open %s\n", argv[1]); return 2; }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) { std::printf("CFB parse failed\n"); return 2; }

    const QStringList base = { QStringLiteral("   214"), QStringLiteral("CV_iew") };
    const QByteArray fixedMeta = cf.readStream(base + QStringList{ QStringLiteral("FixedMeta") });
    const QByteArray fixedData = cf.readStream(base + QStringList{ QStringLiteral("FixedData") });
    const QByteArray varMeta = cf.readStream(base + QStringList{ QStringLiteral("VarMeta") });
    const QByteArray var2 = cf.readStream(base + QStringList{ QStringLiteral("Var2Data") });
    if (varMeta.size() < 24) { std::printf("no CV_iew VarMeta\n"); return 1; }

    const int itemCount = int(u32(varMeta, 8));

    // Find the first non-split viewType==1 view = "Gantt Chart".
    int ganttUid = -1;
    const int fmItems = fixedMeta.size() >= 16 ? int(u32(fixedMeta, 8)) : 0;
    int lastOffset = -1;
    for (int i = 0; i < fmItems && ganttUid < 0; ++i) {
        const int metaOff = 16 + i * 10;
        if (metaOff + 10 > fixedMeta.size()) break;
        const int offset = u16(fixedMeta, metaOff + 4);
        if (offset <= lastOffset) continue;
        lastOffset = offset;
        if (offset + 138 > fixedData.size()) continue;
        if (u16(fixedData, offset + 110) == 0 && u16(fixedData, offset + 112) == 1)
            ganttUid = int(u32(fixedData, offset + 0));
    }
    if (ganttUid < 0) { std::printf("no Gantt Chart view\n"); return 1; }

    // Its PROPERTIES (type 6) Var2Data blob.
    int propsOff = -1, propsLen = -1;
    for (int i = 0; i < itemCount; ++i) {
        const int o = 24 + i * 12;
        if (o + 12 > varMeta.size()) break;
        if (int(u32(varMeta, o)) == ganttUid && u16(varMeta, o + 8) == 6) {
            propsOff = int(u32(varMeta, o + 4));
            propsLen = int(u32(var2, propsOff));
            break;
        }
    }
    if (propsOff < 0) { std::printf("Gantt view has no PROPERTIES record\n"); return 1; }
    const QByteArray props = var2.mid(propsOff + 4, propsLen);

    // Walk Props9 items to STYLE_DATA (574619656).
    const int pc = u16(props, 12);
    int styleOff = -1, styleSize = 0;
    int po = 16;
    for (int i = 0; i < pc; ++i) {
        if (po + 12 > props.size()) break;
        const quint32 size = u32(props, po);
        const quint32 key = u32(props, po + 4);
        po += 12;
        if (key == 574619656u) { styleOff = po; styleSize = int(size); break; }
        po += int(size);
        if (size % 2 != 0) ++po;
    }
    if (styleOff < 0) { std::printf("no STYLE_DATA item\n"); return 1; }

    const QByteArray sd = props.mid(styleOff, styleSize);
    std::printf("file: %s\n", argv[1]);
    std::printf("Gantt view uid=%d  STYLE_DATA size=%d\n", ganttUid, styleSize);

    constexpr int kCount = 2243, kBase = 2255, kRec = 195;
    const int barCount = u8(sd, kCount);
    const int arrayEnd = kBase + barCount * kRec;
    std::printf("barCount@2243=%d  array=[%d..%d]  tail=[%d..%d] (%d bytes undecoded)\n\n",
                barCount, kBase, arrayEnd, arrayEnd, styleSize, styleSize - arrayEnd);

    // Hypothesised 195-byte layout (from reference_mpp_view_formatting_layout memory
    // note / MPXJ GanttChartView14) -- printing every field so it can be checked.
    for (int i = 0; i < barCount; ++i) {
        const int o = kBase + i * kRec;
        if (o + kRec > sd.size()) { std::printf("record %d out of bounds\n", i); break; }
        QString name;
        for (int b = 91; b + 1 < kRec; b += 2) {
            const ushort ch = u16(sd, o + b);
            if (ch == 0) break;
            name.append(QChar(ch));
        }
        std::printf("[%2d] \"%s\"\n", i, qPrintable(name));
        std::printf("     midShape@+0=%u  midPat@+1=%u  midColor@+2=%s\n",
                    u8(sd, o + 0), u8(sd, o + 1), qPrintable(colorAt(sd, o + 2)));
        std::printf("     startST@+15=%u (%%25 shape=%u type=%u | %%21 shape=%u type=%u)  startColor@+16=%s\n",
                    u8(sd, o + 15), u8(sd, o + 15) % 25, u8(sd, o + 15) / 25,
                    u8(sd, o + 15) % 21, u8(sd, o + 15) / 21, qPrintable(colorAt(sd, o + 16)));
        std::printf("     endST@+28=%u (%%25 shape=%u type=%u)  endColor@+29=%s\n",
                    u8(sd, o + 28), u8(sd, o + 28) % 25, u8(sd, o + 28) / 25, qPrintable(colorAt(sd, o + 29)));
        std::printf("     from@+41=%s  to@+45=%s\n",
                    qPrintable(fieldId(u32(sd, o + 41))), qPrintable(fieldId(u32(sd, o + 45))));
        std::printf("     showFor@+49=0x%016llx  showForNot@+57=0x%016llx  row@+65=%u\n",
                    (unsigned long long)u64(sd, o + 49), (unsigned long long)u64(sd, o + 57), u16(sd, o + 65));
        std::printf("     barText L@+67=%s R@+71=%s T@+75=%s B@+79=%s I@+83=%s  id@+89=%u\n",
                    qPrintable(fieldId(u32(sd, o + 67))), qPrintable(fieldId(u32(sd, o + 71))),
                    qPrintable(fieldId(u32(sd, o + 75))), qPrintable(fieldId(u32(sd, o + 79))),
                    qPrintable(fieldId(u32(sd, o + 83))), u16(sd, o + 89));
    }

    // Verify readRecord/writeRecord are byte-exact inverses on this real file:
    // decode each record, re-encode it into a scratch copy, diff.
    {
        int mismatchRecords = 0, mismatchBytes = 0;
        for (int i = 0; i < barCount; ++i) {
            const int o = kBase + i * kRec;
            if (o + kRec > sd.size())
                break;
            const schedule::ViewBarStyle bs = BarStyleCodec::readRecord(sd, o);
            QByteArray scratch = sd;
            BarStyleCodec::writeRecord(scratch, o, bs);
            bool recBad = false;
            for (int b = 0; b < kRec; ++b) {
                if (scratch.at(o + b) != sd.at(o + b)) {
                    ++mismatchBytes;
                    if (!recBad)
                        std::printf("  record %d (\"%s\") re-encode differs:", i,
                                    qPrintable(bs.name));
                    std::printf(" +%d(%02x->%02x)", b, u8(sd, o + b), u8(scratch, o + b));
                    recBad = true;
                }
            }
            if (recBad) { std::printf("\n"); ++mismatchRecords; }
        }
        std::printf("\n=== codec self-check: %d/%d records re-encode byte-exact (%d byte diffs) ===\n",
                    barCount - mismatchRecords, barCount, mismatchBytes);
    }

    std::printf("\n=== raw hex, first %d record(s) ===\n", hexRecords);
    for (int i = 0; i < hexRecords && i < barCount; ++i)
        hexDump(sd, kBase + i * kRec, kRec, qPrintable(QStringLiteral("record %1").arg(i)));

    std::printf("\n=== tail: first 256 bytes after the array ===\n");
    hexDump(sd, arrayEnd, qMin(256, styleSize - arrayEnd), "tail");

    // Scan the whole tail for non-zero content.
    int firstNz = -1, lastNz = -1, nzCount = 0;
    for (int i = arrayEnd; i < styleSize; ++i) {
        if (u8(sd, i) != 0) {
            if (firstNz < 0) firstNz = i;
            lastNz = i;
            ++nzCount;
        }
    }
    std::printf("\ntail scan [%d..%d]: non-zero bytes=%d", arrayEnd, styleSize, nzCount);
    if (firstNz >= 0) {
        std::printf("  firstNZ=%d (rec-rel +%d)  lastNZ=%d\n", firstNz, firstNz - arrayEnd, lastNz);
        hexDump(sd, qMax(arrayEnd, firstNz - 16), qMin(160, styleSize - qMax(arrayEnd, firstNz - 16)), "around firstNZ");
    } else {
        std::printf("  (tail is entirely zero)\n");
    }

    // Also scan the region BEFORE kBase (bytes 0..2255: text styles + gridlines +
    // whatever sits between gridlines@~1057 and barCount@2243).
    std::printf("\n=== pre-array region 1090..2255 (between gridlines and bar array) ===\n");
    hexDump(sd, 1090, 2255 - 1090, "pre-array");

    return 0;
}
