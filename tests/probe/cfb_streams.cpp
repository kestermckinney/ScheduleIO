// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic: print "path|size|hash" for every stream in a .mpp, so two files
// can be diffed to find which stream(s) a change landed in.
// usage: cfb_streams <file.mpp>

#include "ole/compoundfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <cstdio>

static void walk(const CompoundFile &cf, const QStringList &path)
{
    for (const QString &name : cf.childNames(path)) {
        QStringList child = path;
        child << name;
        if (cf.hasStorage(child)) {
            walk(cf, child);
        } else {
            const QByteArray d = cf.readStream(child);
            const QByteArray h = QCryptographicHash::hash(d, QCryptographicHash::Md5).toHex();
            std::printf("%s|%d|%s\n", child.join(QLatin1Char('/')).toUtf8().constData(),
                        d.size(), h.constData());
        }
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: cfb_streams <file.mpp>\n"); return 2; }
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open\n"); return 2; }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) { std::printf("parse failed\n"); return 2; }
    walk(cf, {});
    return 0;
}
