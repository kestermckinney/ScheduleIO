// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Diagnostic: copy <base.mpp> to <out.mpp>, replacing every stream whose
// slash path starts with one of the given prefixes by the donor's stream.
// Streams may change size (the docfile is rebuilt), so whole tables can be
// transplanted between an MS Project file and our own to bisect a rejection.
// usage: swap_streams <base.mpp> <donor.mpp> <out.mpp> <prefix> [<prefix>...]
//   e.g. swap_streams sv.mpp msp.mpp out.mpp "   114/TBkndAssn/"
#include "src/ole/compoundfile.h"

#include <QFile>
#include <cstdio>

static QByteArray readAll(const char *path)
{
    QFile f(QString::fromLocal8Bit(path));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static void copyTree(const CompoundFile &base, const CompoundFile &donor,
                     CompoundFile &out, const QStringList &storage,
                     const QStringList &prefixes)
{
    for (const QString &name : base.childNames(storage)) {
        const QStringList path = storage + QStringList{name};
        if (base.hasStorage(path)) {
            out.addStorage(path);
            copyTree(base, donor, out, path, prefixes);
            continue;
        }
        const QString slash = path.join('/');
        bool swap = false;
        for (const QString &p : prefixes)
            swap = swap || slash.startsWith(p);
        if (swap && donor.hasStream(path)) {
            printf("swap %s (%lld -> %lld)\n", qPrintable(slash),
                   qint64(base.readStream(path).size()), qint64(donor.readStream(path).size()));
            out.addStream(path, donor.readStream(path));
        } else {
            out.addStream(path, base.readStream(path));
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: swap_streams <base.mpp> <donor.mpp> <out.mpp> <prefix>...\n");
        return 2;
    }
    CompoundFile base, donor, out;
    if (!base.openFromData(readAll(argv[1])) || !donor.openFromData(readAll(argv[2]))) {
        fprintf(stderr, "cannot open input docfiles\n");
        return 1;
    }
    QStringList prefixes;
    for (int i = 4; i < argc; ++i)
        prefixes << QString::fromLocal8Bit(argv[i]);
    copyTree(base, donor, out, {}, prefixes);
    QFile f(QString::fromLocal8Bit(argv[3]));
    if (!f.open(QIODevice::WriteOnly) || f.write(out.toByteArray()) < 0) {
        fprintf(stderr, "cannot write %s\n", argv[3]);
        return 1;
    }
    printf("wrote %s\n", argv[3]);
    return 0;
}
