// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Throwaway diagnostic: dumps the raw TBkndTask streams of a fixture so the
// FixedMeta/FixedData/Var2Data layouts can be reverse-engineered. Not a unit
// test -- run it manually with a fixture path argument.

#include "ole/compoundfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QMap>
#include <QStringList>
#include <QtEndian>
#include <cstdio>

static void hexDump(const QByteArray &d, int maxBytes)
{
    const int n = qMin(d.size(), maxBytes);
    for (int i = 0; i < n; i += 16) {
        std::printf("%06x  ", i);
        for (int j = 0; j < 16; ++j) {
            if (i + j < n) std::printf("%02x ", static_cast<uchar>(d.at(i + j)));
            else std::printf("   ");
        }
        std::printf(" |");
        for (int j = 0; j < 16 && i + j < n; ++j) {
            const uchar c = static_cast<uchar>(d.at(i + j));
            std::printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        std::printf("|\n");
    }
}

static QByteArray rd(const CompoundFile &cf, const QString &entity, const QString &stream)
{
    return cf.readStream({ QStringLiteral("   114"), entity, stream });
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: dump_task <file.mpp>\n"); return 2; }

    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open\n"); return 2; }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) { std::printf("parse failed: %s\n",
                                                     qPrintable(cf.errorString())); return 2; }

    const QString entity = QStringLiteral("TBkndTask");
    for (const QString &s : { QStringLiteral("FixedMeta"), QStringLiteral("FixedData"),
                              QStringLiteral("VarMeta"), QStringLiteral("Var2Data"),
                              QStringLiteral("Fixed2Meta"), QStringLiteral("Fixed2Data"),
                              QStringLiteral("Props") }) {
        const QByteArray b = rd(cf, entity, s);
        std::printf("\n==== %s/%s : %d bytes ====\n", qPrintable(entity), qPrintable(s), b.size());
        if (!b.isEmpty()) {
            const uchar *p = reinterpret_cast<const uchar *>(b.constData());
            if (b.size() >= 8)
                std::printf("first dwords: %u %u\n",
                            qFromLittleEndian<quint32>(p), qFromLittleEndian<quint32>(p + 4));
        }
    }

    std::printf("\n---- FixedMeta (full) ----\n");      hexDump(rd(cf, entity, "FixedMeta"), 4096);
    std::printf("\n---- FixedData (first 512) ----\n");  hexDump(rd(cf, entity, "FixedData"), 512);
    std::printf("\n---- VarMeta (first 256) ----\n");    hexDump(rd(cf, entity, "VarMeta"), 256);
    std::printf("\n---- Var2Data (first 256) ----\n");   hexDump(rd(cf, entity, "Var2Data"), 256);

    // Hypothesis check: VarMeta 12-byte records [u16 item][u16 type][u32][u32 off];
    // field type 0x0b40 == task Name, stored in Var2Data as [u32 len][UTF-16].
    const QByteArray vm = rd(cf, entity, "VarMeta");
    const QByteArray v2 = rd(cf, entity, "Var2Data");
    const uchar *vp = reinterpret_cast<const uchar *>(vm.constData());
    // Generic: scan every VarMeta record, decode any whose blob looks like a
    // UTF-16 text string, and tally which field code holds the most strings.
    std::printf("\n---- string-bearing field codes (code: count) ----\n");
    const uchar *v2p = reinterpret_cast<const uchar *>(v2.constData());
    QMap<quint16, int> codeCounts;
    QMap<quint16, QString> sample;
    for (int o = 32; o + 12 <= vm.size(); o += 12) {
        const quint16 code = qFromLittleEndian<quint16>(vp + o);
        const quint32 off  = qFromLittleEndian<quint32>(vp + o + 8);
        if (off + 4 > quint32(v2.size())) continue;
        const quint32 len = qFromLittleEndian<quint32>(v2p + off);
        if (len < 4 || len > 200 || off + 4 + len > quint32(v2.size())) continue;
        // Heuristic: UTF-16 ASCII text has zero high bytes and printable lows.
        bool looksText = true;
        for (quint32 k = 0; k + 1 < len && looksText; k += 2) {
            const uchar lo = v2p[off + 4 + k], hi = v2p[off + 4 + k + 1];
            if (hi != 0 || lo < 0x20 || lo > 0x7e) {
                if (!(lo == 0 && hi == 0)) looksText = false;   // allow NUL terminator
            }
        }
        if (!looksText) continue;
        QString s = QString::fromUtf16(reinterpret_cast<const char16_t *>(v2p + off + 4), len / 2);
        while (s.endsWith(QChar(u'\0'))) s.chop(1);
        if (s.size() < 2) continue;
        codeCounts[code]++;
        if (!sample.contains(code)) sample[code] = s;
    }
    for (auto it = codeCounts.constBegin(); it != codeCounts.constEnd(); ++it)
        std::printf("  code %-5u : %-3d  e.g. \"%s\"\n", it.key(), it.value(),
                    qPrintable(sample.value(it.key())));

    // If a search string is given, find it in Var2Data and report which VarMeta
    // record(s) point at that offset (decisive for identifying the Name code).
    // Numeric arg: dump all VarMeta records whose type == N, using MPXJ's
    // VarMeta12 layout: 24-byte header, then 12-byte records
    // [u32 uniqueID][u32 offset][u16 type][u16 unknown].
    if (argc >= 3 && QByteArray(argv[2]).toInt() != 0) {
        const quint16 wantType = quint16(QByteArray(argv[2]).toInt());
        std::printf("\n---- records with type==%u (MPXJ layout) ----\n", wantType);
        int n = 0;
        for (int o = 24; o + 12 <= vm.size(); o += 12) {
            const quint32 uid  = qFromLittleEndian<quint32>(vp + o);
            const quint32 off  = qFromLittleEndian<quint32>(vp + o + 4);
            const quint16 type = qFromLittleEndian<quint16>(vp + o + 8);
            if (type != wantType) continue;
            ++n;
            quint32 len = (off + 4 <= quint32(v2.size())) ? qFromLittleEndian<quint32>(v2p + off) : 0;
            QString s;
            if (len >= 2 && off + 4 + len <= quint32(v2.size()))
                s = QString::fromUtf16(reinterpret_cast<const char16_t *>(v2p + off + 4), len / 2);
            while (s.endsWith(QChar(u'\0'))) s.chop(1);
            if (!s.isEmpty())
                std::printf("NAME14\t%s\n", qPrintable(s));
        }
        std::printf("  total type-%u records: %d\n", wantType, n);
        return 0;
    }

    if (argc >= 3) {
        const QString needle = QString::fromLocal8Bit(argv[2]);
        const QByteArray needle16(reinterpret_cast<const char *>(needle.utf16()), needle.size() * 2);
        std::printf("\n---- search '%s' ----\n", qPrintable(needle));
        // Report every Var2Data offset where the string occurs.
        for (int from = 0; (from = v2.indexOf(needle16, from)) >= 0; from += 2)
            std::printf("  occurs at Var2Data offset %d (blob len-prefix at %d)\n", from, from - 4);
        // Decode each VarMeta record's blob; report records whose text == needle.
        for (int o = 32; o + 12 <= vm.size(); o += 12) {
            const quint16 code = qFromLittleEndian<quint16>(vp + o);
            const quint16 item = qFromLittleEndian<quint16>(vp + o + 4);
            const quint32 off  = qFromLittleEndian<quint32>(vp + o + 8);
            if (off + 4 > quint32(v2.size())) continue;
            const quint32 len = qFromLittleEndian<quint32>(v2p + off);
            if (len < 4 || len > 400 || off + 4 + len > quint32(v2.size())) continue;
            QString s = QString::fromUtf16(reinterpret_cast<const char16_t *>(v2p + off + 4), len / 2);
            while (s.endsWith(QChar(u'\0'))) s.chop(1);
            if (s == needle)
                std::printf("  --> record: fieldCode=%u itemIndex=%u offset=%u\n", code, item, off);
        }
    }
    return 0;
}
