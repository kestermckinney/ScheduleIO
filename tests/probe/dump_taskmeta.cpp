// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic (throwaway): dump the 47-byte TBkndTask FixedMeta item + first
// 64 bytes of the FixedData block for a specific task uid, to check for a
// meta/data bit that differs when a task has a COLUMN_PROPERTIES exception.
// usage: dump_taskmeta <file.mpp> <uid>

#include "ole/compoundfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <cstdio>

static quint32 u32(const QByteArray &d, int o)
{ return (o < 0 || o + 4 > d.size()) ? 0 : qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(d.constData()) + o); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) { std::printf("usage: dump_taskmeta <file.mpp> <uid>\n"); return 2; }
    const quint32 targetUid = quint32(QByteArray(argv[2]).toUInt());

    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open\n"); return 2; }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) { std::printf("parse failed\n"); return 2; }

    const QByteArray meta = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndTask"), QStringLiteral("FixedMeta") });
    const QByteArray data = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndTask"), QStringLiteral("FixedData") });

    if (meta.size() < 16) { std::printf("no FixedMeta\n"); return 1; }
    const int itemCount = int(u32(meta, 8));
    std::printf("FixedMeta items=%d FixedData=%d bytes\n", itemCount, data.size());

    for (int i = 0; i < itemCount; ++i) {
        const int mo = 16 + i * 47;
        if (mo + 47 > meta.size()) break;
        const quint32 off = u32(meta, mo + 4);
        if (off + 8 > quint32(data.size())) continue;
        const quint32 uid = u32(data, int(off) + 4);   // UNIQUE_ID(86)@4 per DECODING_NOTES
        if (uid != targetUid) continue;

        std::printf("Found task uid=%u at meta item %d, data offset %u\n", uid, i, off);
        std::printf("47-byte FixedMeta item:\n ");
        for (int b = 0; b < 47; ++b)
            std::printf(" %02x", uchar(meta.at(mo + b)));
        std::printf("\n64-byte FixedData block (from offset %u):\n ", off);
        for (int b = 0; b < 64 && int(off) + b < data.size(); ++b)
            std::printf(" %02x", uchar(data.at(int(off) + b)));
        std::printf("\n");
        return 0;
    }
    std::printf("uid %u not found\n", targetUid);
    return 1;
}
