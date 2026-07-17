// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Print the decoded in-memory model of a .mpp (or .xml) at a glance: project
// header, tasks with hierarchy/format, resources, assignments, relations,
// calendars. The first stop when diffing decoded output against a fixture's
// XML export or manifest.
#include "mppio.h"
#include "xmlio.h"

#include <cstdio>

using schedule::Project;
using schedule::TextStyle;

static QString fmtStyle(const TextStyle &s)
{
    if (s.isDefault())
        return QString();
    QStringList bits;
    if (s.bold) bits << QStringLiteral("bold");
    if (s.italic) bits << QStringLiteral("italic");
    if (s.underline) bits << QStringLiteral("underline");
    if (s.strikethrough) bits << QStringLiteral("strike");
    if (s.color != TextStyle::kAutomatic)
        bits << QStringLiteral("color=#%1").arg(s.color, 6, 16, QLatin1Char('0'));
    if (s.backColor != TextStyle::kAutomatic)
        bits << QStringLiteral("back=#%1").arg(s.backColor, 6, 16, QLatin1Char('0'));
    if (s.backPattern != 0)
        bits << QStringLiteral("pattern=%1").arg(s.backPattern);
    return QStringLiteral("  [%1]").arg(bits.join(QLatin1Char(' ')));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: dump_model <file.mpp|file.xml>\n");
        return 2;
    }
    const QString path = QString::fromLocal8Bit(argv[1]);
    Project p;
    if (path.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)) {
        XmlIO io;
        if (!io.open(path)) {
            fprintf(stderr, "read failed: %s\n", qPrintable(io.errorString()));
            return 1;
        }
        p = io.project();
    } else {
        MppIO io;
        if (!io.open(path)) {
            fprintf(stderr, "read failed: %s\n", qPrintable(io.errorString()));
            return 1;
        }
        p = io.project();
    }

    printf("title='%s' author='%s' start=%s finish=%s calUid=%d version=%d\n",
           qPrintable(p.title), qPrintable(p.author),
           qPrintable(p.startDate.toString(Qt::ISODate)),
           qPrintable(p.finishDate.toString(Qt::ISODate)),
           p.calendarUniqueId, int(p.formatVersion));

    printf("-- %lld tasks\n", (long long)p.tasks.size());
    for (const schedule::Task &t : p.tasks) {
        printf("  uid=%-3d id=%-3d L%d %s'%s'%s%s%s dur=%lldm %s..%s pct=%.0f\n",
               t.uniqueId, t.id, t.outlineLevel,
               QByteArray(2 * (t.outlineLevel - 1), ' ').constData(),
               qPrintable(t.name),
               t.summary ? " [sum]" : "", t.milestone ? " [mile]" : "",
               qPrintable(fmtStyle(t.rowFormat)),
               (long long)(t.durationMillis / 60000),
               qPrintable(t.start.toString(Qt::ISODate)),
               qPrintable(t.finish.toString(Qt::ISODate)),
               t.percentComplete);
    }

    printf("-- %lld resources\n", (long long)p.resources.size());
    for (const schedule::Resource &r : p.resources)
        printf("  uid=%-3d '%s' maxUnits=%.2f calUid=%d costRates=%lld\n",
               r.uniqueId, qPrintable(r.name), r.maxUnits, r.calendarUniqueId,
               (long long)r.costRates.size());

    printf("-- %lld assignments\n", (long long)p.assignments.size());
    for (const schedule::Assignment &a : p.assignments)
        printf("  task=%-3d res=%-3d units=%.2f work=%lldm\n",
               a.taskUniqueId, a.resourceUniqueId, a.units,
               (long long)(a.workMillis / 60000));

    printf("-- %lld relations\n", (long long)p.relations.size());
    for (const schedule::Relation &r : p.relations)
        printf("  %d -> %d type=%d lag=%lldm\n", r.predecessorTaskUid,
               r.successorTaskUid, int(r.type), (long long)(r.lagMillis / 60000));

    printf("-- %lld calendars\n", (long long)p.calendars.size());
    for (const schedule::Calendar &c : p.calendars)
        printf("  uid=%-3d '%s' base=%d mask=0x%02x exceptions=%lld\n",
               c.uniqueId, qPrintable(c.name), c.baseCalendarUniqueId,
               unsigned(c.workingDayMask), (long long)c.exceptions.size());

    return 0;
}
