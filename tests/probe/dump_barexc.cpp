// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic: dump a Gantt view's BAR_EXCEPTION_STYLES Props9 item (key
// 574619661) -- MS Project's per-task bar formatting, what Format > Bar writes
// for one task rather than for a whole bar-style category.
// usage: dump_barexc <file.mpp> [recordSize]

#include "ole/compoundfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QtEndian>
#include <cstdio>

static quint32 u32(const QByteArray &d, int o)
{ return (o < 0 || o + 4 > d.size()) ? 0 : qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(d.constData()) + o); }
static quint16 u16(const QByteArray &d, int o)
{ return (o < 0 || o + 2 > d.size()) ? 0 : qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(d.constData()) + o); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: dump_barexc <file.mpp> [recordSize]\n"); return 2; }
    const int wantSize = argc >= 3 ? QString::fromLocal8Bit(argv[2]).toInt() : 0;

    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open\n"); return 2; }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) { std::printf("CFB parse failed\n"); return 2; }

    const QStringList base = { QStringLiteral("   214"), QStringLiteral("CV_iew") };
    const QByteArray fixedMeta = cf.readStream(base + QStringList{ QStringLiteral("FixedMeta") });
    const QByteArray fixedData = cf.readStream(base + QStringList{ QStringLiteral("FixedData") });
    const QByteArray varMeta = cf.readStream(base + QStringList{ QStringLiteral("VarMeta") });
    const QByteArray var2 = cf.readStream(base + QStringList{ QStringLiteral("Var2Data") });
    if (varMeta.size() < 24 || u32(varMeta, 0) != 0xFADFADBAu) { std::printf("bad VarMeta\n"); return 1; }
    const int itemCount = int(u32(varMeta, 8));

    // Every non-split view, so a file with several Gantts shows all of them.
    const int fmItems = fixedMeta.size() >= 16 ? int(u32(fixedMeta, 8)) : 0;
    int lastOffset = -1;
    for (int i = 0; i < fmItems; ++i) {
        const int metaOff = 16 + i * 10;
        if (metaOff + 10 > fixedMeta.size()) break;
        const int offset = u16(fixedMeta, metaOff + 4);
        if (offset <= lastOffset) continue;
        lastOffset = offset;
        if (offset + 138 > fixedData.size()) continue;
        if (u16(fixedData, offset + 110) != 0) continue;   // split view
        const quint32 viewUid = u32(fixedData, offset + 0);
        QString name;
        for (int b = 4; b + 1 < 110; b += 2) {
            const ushort ch = u16(fixedData, offset + b);
            if (ch == 0) break;
            name.append(QChar(ch));
        }

        int propsOff = -1;
        for (int k = 0; k < itemCount; ++k) {
            const int o = 24 + k * 12;
            if (o + 12 > varMeta.size()) break;
            if (u32(varMeta, o) == viewUid && u16(varMeta, o + 8) == 6) {
                propsOff = int(u32(varMeta, o + 4));
                break;
            }
        }
        if (propsOff < 0) continue;
        const QByteArray props = var2.mid(propsOff + 4, int(u32(var2, propsOff)));
        if (props.size() < 16) continue;

        const int pc = u16(props, 12);
        int po = 16;
        for (int k = 0; k < pc; ++k) {
            if (po + 12 > props.size()) break;
            const quint32 size = u32(props, po);
            const quint32 key = u32(props, po + 4);
            po += 12;
            if (key == 574619661u) {
                const QByteArray blob = props.mid(po, int(size));
                std::printf("view uid=%u \"%s\": BAR_EXCEPTION_STYLES size=%u\n",
                            viewUid, qPrintable(name), size);
                for (int rs : { 38, 39, 40, 71 }) {
                    std::printf("  /%d -> %d rem %d%s\n", rs, blob.size() / rs,
                                blob.size() % rs, blob.size() % rs == 0 ? "  <= exact" : "");
                }
                std::printf("  hex:\n");
                for (int o2 = 0; o2 < blob.size(); o2 += 16) {
                    std::printf("   %04X: ", o2);
                    for (int b = 0; b < 16; ++b)
                        if (o2 + b < blob.size()) std::printf("%02X ", uchar(blob.at(o2 + b)));
                        else std::printf("   ");
                    std::printf("\n");
                }
                if (wantSize > 0) {
                    std::printf("  as %d-byte records:\n", wantSize);
                    for (int r = 0; r + wantSize <= blob.size(); r += wantSize) {
                        std::printf("   [%2d] uid=%-6u styleId=%-4u ", r / wantSize,
                                    u32(blob, r), u16(blob, r + 4));
                        for (int b = 6; b < wantSize; ++b)
                            std::printf("%02X ", uchar(blob.at(r + b)));
                        std::printf("\n");
                    }
                }
            }
            po += int(size);
            if (size % 2 != 0) ++po;
        }
    }
    return 0;
}
