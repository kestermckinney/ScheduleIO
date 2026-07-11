// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Diagnostic: print every calendar in an .mpp (uid, name, base, day mask,
// per-weekday working times, exception count) plus the project default and
// each resource's calendar link. Ground truth for the default-calendars work.
#include "mppio.h"

#include <cstdio>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: dump_cal <in.mpp>\n");
        return 2;
    }
    MppIO io;
    if (!io.open(QString::fromLocal8Bit(argv[1]))) {
        fprintf(stderr, "read failed: %s\n", qPrintable(io.errorString()));
        return 1;
    }
    const schedule::Project &p = io.project();
    printf("project defaultCalendarUid=%d, %lld calendars, %lld resources\n",
           p.calendarUniqueId, (long long)p.calendars.size(), (long long)p.resources.size());

    static const char *dayName[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    for (const schedule::Calendar &c : p.calendars) {
        printf("calendar uid=%-3d base=%-3d mask=0x%02x name=\"%s\" exceptions=%lld\n",
               c.uniqueId, c.baseCalendarUniqueId, c.workingDayMask,
               qPrintable(c.name), (long long)c.exceptions.size());
        for (int d = 0; d < c.workingTimes.size() && d < 7; ++d) {
            if (c.workingTimes[d].isEmpty())
                continue;
            printf("  %s:", dayName[d]);
            for (const schedule::TimeRange &tr : c.workingTimes[d])
                printf(" %s-%s", qPrintable(tr.start.toString("HH:mm")),
                       qPrintable(tr.end.toString("HH:mm")));
            printf("\n");
        }
    }
    for (const schedule::Resource &r : p.resources)
        printf("resource uid=%-3d calendarUid=%-3d name=\"%s\"\n",
               r.uniqueId, r.calendarUniqueId, qPrintable(r.name));
    return 0;
}
