// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Throwaway diagnostic: opens a .mpp/.xml via MppIO/XmlIO and prints every
// task (uid/id/name) plus every relation (pred -> succ, with names), so a
// user-reported circular-reference error can be traced back to the exact
// link causing it.

#include "mppio.h"
#include "xmlio.h"

#include <QCoreApplication>
#include <QHash>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: dump_relations <file.mpp|.xml>\n"); return 2; }

    QString path = QString::fromLocal8Bit(argv[1]);
    schedule::Project p;
    if (path.endsWith(".xml", Qt::CaseInsensitive)) {
        XmlIO io;
        if (!io.open(path)) { std::printf("read failed: %s\n", qPrintable(io.errorString())); return 1; }
        p = io.project();
    } else {
        MppIO io;
        if (!io.open(path)) { std::printf("read failed: %s\n", qPrintable(io.errorString())); return 1; }
        p = io.project();
    }

    QHash<int, QString> nameByUid;
    QHash<int, int> idByUid;
    for (const schedule::Task &t : p.tasks) {
        nameByUid[t.uniqueId] = t.name;
        idByUid[t.uniqueId] = t.id;
    }

    std::printf("---- tasks (%lld) ----\n", (long long)p.tasks.size());
    for (const schedule::Task &t : p.tasks) {
        std::printf("  id=%d uid=%d outline=%d summary=%d name=\"%s\"\n",
                     t.id, t.uniqueId, t.outlineLevel, t.summary ? 1 : 0, qPrintable(t.name));
    }

    std::printf("---- relations (%lld) ----\n", (long long)p.relations.size());
    for (const schedule::Relation &r : p.relations) {
        std::printf("  consUid=%d  pred uid=%d (id=%d \"%s\")  ->  succ uid=%d (id=%d \"%s\")  type=%d lagMillis=%lld\n",
                     r.uniqueId,
                     r.predecessorTaskUid, idByUid.value(r.predecessorTaskUid, -1),
                     qPrintable(nameByUid.value(r.predecessorTaskUid, "?")),
                     r.successorTaskUid, idByUid.value(r.successorTaskUid, -1),
                     qPrintable(nameByUid.value(r.successorTaskUid, "?")),
                     r.type, (long long)r.lagMillis);
    }
    return 0;
}
