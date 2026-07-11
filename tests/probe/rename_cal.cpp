// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Diagnostic: rename a calendar by uid and resave, so MS Project's handling of
// our base-calendar records can be probed (name-dedupe vs record rejection).
// usage: rename_cal <in.mpp> <out.mpp> <uid:newname> [<uid:newname> ...]
#include "mppio.h"

#include <cstdio>

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: rename_cal <in.mpp> <out.mpp> <uid:newname>...\n");
        return 2;
    }
    MppIO io;
    if (!io.open(QString::fromLocal8Bit(argv[1]))) {
        fprintf(stderr, "read failed: %s\n", qPrintable(io.errorString()));
        return 1;
    }
    schedule::Project p = io.project();
    for (int i = 3; i < argc; ++i) {
        const QString spec = QString::fromLocal8Bit(argv[i]);
        const int uid = spec.section(':', 0, 0).toInt();
        const QString name = spec.section(':', 1);
        for (schedule::Calendar &c : p.calendars)
            if (c.uniqueId == uid) {
                printf("uid %d: \"%s\" -> \"%s\"\n", uid, qPrintable(c.name), qPrintable(name));
                c.name = name;
            }
    }
    io.setProject(p);
    if (!io.save(QString::fromLocal8Bit(argv[2]))) {
        fprintf(stderr, "save failed: %s\n", qPrintable(io.errorString()));
        return 1;
    }
    printf("saved %s\n", argv[2]);
    return 0;
}
