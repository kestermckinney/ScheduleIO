// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Diagnostic: print the view / table settings the reader decodes for the
// Gantt Chart, Task Usage, Resource Usage and Team Planner views.
// usage: dump_views <file.mpp>
#include "mppio.h"

#include <cstdio>

static void dump(const char *label, const schedule::UsageViewSettings &v)
{
    printf("%s: present=%d view='%s' table='%s' tableWidth=%d details=%lld\n", label,
           int(v.present), qPrintable(v.viewName), qPrintable(v.tableName), v.tableWidth,
           qint64(v.detailFields.size()));
    for (const schedule::UsageTableColumn &c : v.columns)
        printf("    field=0x%08x width=%d title='%s'\n", c.fieldId, c.width, qPrintable(c.title));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: dump_views <file.mpp>\n");
        return 2;
    }
    MppIO io;
    if (!io.open(QString::fromLocal8Bit(argv[1]))) {
        fprintf(stderr, "read failed: %s\n", qPrintable(io.errorString()));
        return 1;
    }
    const schedule::Project &p = io.project();
    dump("Gantt Chart", p.ganttView);
    dump("Task Usage", p.taskUsageView);
    dump("Resource Usage", p.resourceUsageView);
    dump("Team Planner", p.teamPlannerView);
    return 0;
}
