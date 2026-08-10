// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Round-trip a .mpp through the library: read `in`, save as real MPP14 to
// `out`. For external validation (MPXJ, MS Project) of the writer's output.
#include "mppio.h"

#include <cstdio>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: resave <in.mpp> <out.mpp>\n");
        return 2;
    }
    MppIO io;
    if (!io.open(QString::fromLocal8Bit(argv[1]))) {
        fprintf(stderr, "read failed: %s\n", qPrintable(io.errorString()));
        return 1;
    }
    const schedule::Project p = io.project();
    printf("read: %lld tasks, %lld resources, %lld assignments, %lld relations, %lld calendars\n",
           (long long)p.tasks.size(), (long long)p.resources.size(),
           (long long)p.assignments.size(), (long long)p.relations.size(),
           (long long)p.calendars.size());
    // Force the semantic writer. MppIO intentionally returns the untouched
    // source bytes when a loaded project is unchanged, which is correct for
    // production but defeats this writer diagnostic.
    io.setProject(p);
    if (!io.save(QString::fromLocal8Bit(argv[2]))) {
        fprintf(stderr, "save failed: %s\n", qPrintable(io.errorString()));
        return 1;
    }
    printf("saved %s\n", argv[2]);
    return 0;
}
