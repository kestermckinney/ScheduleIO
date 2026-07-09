// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic (throwaway): dump the FONT_BASES table from "   114/Props".
// usage: dump_fontbases <file.mpp>

#include "ole/compoundfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <cstdio>

static quint32 u32(const QByteArray &d, int o)
{ return (o < 0 || o + 4 > d.size()) ? 0 : qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(d.constData()) + o); }
static quint16 u16(const QByteArray &d, int o)
{ return (o < 0 || o + 2 > d.size()) ? 0 : qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(d.constData()) + o); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: dump_fontbases <file.mpp>\n"); return 2; }

    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open\n"); return 2; }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) { std::printf("CFB parse failed\n"); return 2; }

    const QByteArray props = cf.readStream({ QStringLiteral("   114"), QStringLiteral("Props") });
    if (props.size() < 16) { std::printf("no   114/Props\n"); return 1; }

    QByteArray fontBases;
    for (int o = 16; o + 12 <= props.size(); ) {
        const quint32 len = u32(props, o), key = u32(props, o + 4);
        o += 12;
        if (len > quint32(props.size() - o)) break;
        if (key == 54525952u) fontBases = props.mid(o, int(len));
        o += int(len);
    }

    if (fontBases.isEmpty()) { std::printf("no FONT_BASES key found\n"); return 1; }

    const int count = u16(fontBases, 0);
    std::printf("FONT_BASES: %d bytes, count=%d\n", fontBases.size(), count);
    int o = 2;
    for (int i = 0; i < count; ++i) {
        if (o + 68 > fontBases.size()) { std::printf("  [%d]: truncated!\n", i); break; }
        QString name;
        for (int c = 0; c < 32; ++c) {
            ushort ch = u16(fontBases, o + 4 + c * 2);
            if (ch == 0) break;
            name.append(QChar(ch));
        }
        std::printf("  [%d]: %s\n", i, qPrintable(name));
        o += 68;
    }
    return 0;
}
