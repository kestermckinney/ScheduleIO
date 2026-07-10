// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic: dump the raw TBkndRsc stream quartet (FixedMeta/FixedData/
// Fixed2Meta/Fixed2Data/VarMeta/Var2Data) so a ScheduleIO resave can be
// byte-compared against real MS Project ground truth (empty-resource-names RE).
// usage: dump_rsc <file.mpp>

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

static void hexLine(const QByteArray &b, int off, int len)
{
    for (int i = 0; i < len && off + i < b.size(); ++i)
        std::printf("%02x ", static_cast<unsigned char>(b.at(off + i)));
    std::printf("\n");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: dump_rsc <file.mpp>\n"); return 2; }

    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open\n"); return 2; }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) { std::printf("parse failed\n"); return 2; }

    const QString dir = QStringLiteral("   114"), sub = QStringLiteral("TBkndRsc");
    const QByteArray fm  = cf.readStream({ dir, sub, QStringLiteral("FixedMeta") });
    const QByteArray fd  = cf.readStream({ dir, sub, QStringLiteral("FixedData") });
    const QByteArray f2m = cf.readStream({ dir, sub, QStringLiteral("Fixed2Meta") });
    const QByteArray f2d = cf.readStream({ dir, sub, QStringLiteral("Fixed2Data") });
    const QByteArray vm  = cf.readStream({ dir, sub, QStringLiteral("VarMeta") });
    const QByteArray vd  = cf.readStream({ dir, sub, QStringLiteral("Var2Data") });

    std::printf("=== sizes: FixedMeta=%d FixedData=%d Fixed2Meta=%d Fixed2Data=%d VarMeta=%d Var2Data=%d ===\n",
                int(fm.size()), int(fd.size()), int(f2m.size()), int(f2d.size()),
                int(vm.size()), int(vd.size()));

    constexpr int kMetaItem = 37, kRec = 172, kF2MetaItem = 51;

    std::printf("--- FixedMeta header: ");
    hexLine(fm, 0, 16);
    const int items = (fm.size() >= 16) ? (fm.size() - 16) / kMetaItem : 0;
    for (int i = 0; i < items; ++i) {
        const int o = 16 + i * kMetaItem;
        std::printf("item %-2d flags=0x%08x off=%-6u tail: ", i, u32(fm, o), u32(fm, o + 4));
        hexLine(fm, o + 8, kMetaItem - 8);
        const quint32 off = u32(fm, o + 4);
        if (off + 16 <= quint32(fd.size())) {
            const bool full = off + kRec <= quint32(fd.size());
            std::printf("        FixedData@%-6u uid=%-6u id=%-4u first48: ",
                        off, full ? u32(fd, int(off) + 4) : 0u, full ? u32(fd, int(off)) : 0u);
            hexLine(fd, int(off), 48);
        }
    }

    std::printf("--- Fixed2Meta header: ");
    hexLine(f2m, 0, 16);
    const int f2items = (f2m.size() >= 16) ? (f2m.size() - 16) / kF2MetaItem : 0;
    for (int i = 0; i < f2items; ++i) {
        const int o = 16 + i * kF2MetaItem;
        std::printf("f2 item %-2d flags=0x%08x off=%-6u tail: ", i, u32(f2m, o), u32(f2m, o + 4));
        hexLine(f2m, o + 8, kF2MetaItem - 8);
        const quint32 off = u32(f2m, o + 4);
        if (off + 40 <= quint32(f2d.size())) {
            std::printf("        Fixed2Data@%-6u first40: ", off);
            hexLine(f2d, int(off), 40);
        }
    }

    std::printf("--- VarMeta header: ");
    hexLine(vm, 0, 24);
    for (int o = 24; o + 12 <= vm.size(); o += 12) {
        const quint32 uid = u32(vm, o), off = u32(vm, o + 4);
        const quint16 type = u16(vm, o + 8), high = u16(vm, o + 10);
        std::printf("var uid=%-6u off=%-7u type=%-5u high=0x%04x", uid, off, type, high);
        if (off + 4 <= quint32(vd.size())) {
            const quint32 len = u32(vd, int(off));
            std::printf("  [len=%-4u] ", len);
            hexLine(vd, int(off) + 4, int(qMin<quint32>(len, 24)));
        } else {
            std::printf("  [offset out of range]\n");
        }
    }
    return 0;
}
