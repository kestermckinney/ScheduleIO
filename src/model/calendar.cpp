// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/calendar.h"
#include "model/project.h"

#include <QHash>
#include <QSet>

namespace schedule {

bool Calendar::operator==(const Calendar &o) const
{
    return uniqueId == o.uniqueId
        && name == o.name
        && baseCalendarUniqueId == o.baseCalendarUniqueId
        && workingDayMask == o.workingDayMask
        && workingTimes == o.workingTimes
        && exceptions == o.exceptions;
}

QList<Calendar> Calendar::microsoftDefaults()
{
    // Working-time ranges ending at QTime(0, 0) mean "until midnight", the
    // library-wide convention (WorkCalendar::toPeriods, the MPP/MSPDI codecs).
    const TimeRange morning{ QTime(8, 0), QTime(12, 0) };
    const TimeRange afternoon{ QTime(13, 0), QTime(17, 0) };
    const TimeRange fullDay{ QTime(0, 0), QTime(0, 0) };
    const TimeRange smallHours{ QTime(0, 0), QTime(3, 0) };
    const TimeRange earlyShift{ QTime(4, 0), QTime(8, 0) };
    const TimeRange lateShift{ QTime(23, 0), QTime(0, 0) };

    // Verified against real Microsoft Project files (Standard) and Microsoft's
    // documented definitions (24 Hours, Night Shift). Day masks are
    // Monday(bit0)..Sunday(bit6); workingTimes index 0=Monday..6=Sunday.
    Calendar standard;
    standard.uniqueId = 1;
    standard.name = QStringLiteral("Standard");
    standard.workingDayMask = 0x1F;   // Mon-Fri
    standard.workingTimes = { { morning, afternoon }, { morning, afternoon },
                              { morning, afternoon }, { morning, afternoon },
                              { morning, afternoon }, {}, {} };

    Calendar allHours;
    allHours.uniqueId = 2;
    allHours.name = QStringLiteral("24 Hours");
    allHours.workingDayMask = 0x7F;   // every day
    allHours.workingTimes = { { fullDay }, { fullDay }, { fullDay }, { fullDay },
                              { fullDay }, { fullDay }, { fullDay } };

    Calendar nightShift;
    nightShift.uniqueId = 3;
    nightShift.name = QStringLiteral("Night Shift");
    nightShift.workingDayMask = 0x3F;   // Mon-Sat
    nightShift.workingTimes = { { lateShift },
                                { smallHours, earlyShift, lateShift },
                                { smallHours, earlyShift, lateShift },
                                { smallHours, earlyShift, lateShift },
                                { smallHours, earlyShift, lateShift },
                                { smallHours, earlyShift }, {} };

    return { standard, allHours, nightShift };
}

namespace {

bool customizesNothing(const Calendar &c)
{
    for (const QList<TimeRange> &day : c.workingTimes)
        if (!day.isEmpty())
            return false;
    return c.exceptions.isEmpty();
}

} // namespace

void materializeResourceCalendars(Project &project)
{
    // Snapshot keeps the original buffer alive while we append to the live
    // list (appending would otherwise invalidate pointers into it).
    const QList<Calendar> original = project.calendars;
    QHash<int, const Calendar *> calByUid;
    int nextUid = 1;
    for (const Calendar &c : original) {
        calByUid.insert(c.uniqueId, &c);
        nextUid = qMax(nextUid, c.uniqueId + 1);
    }

    for (Resource &r : project.resources) {
        if (r.calendarUniqueId < 0)
            continue;   // "(None)": no row today either; MS Project self-heals
        const Calendar *pointed = calByUid.value(r.calendarUniqueId);
        if (!pointed || pointed->baseCalendarUniqueId >= 0)
            continue;   // unknown uid, or already a derived calendar

        Calendar cal;
        cal.uniqueId = nextUid++;
        cal.name = r.name;
        cal.baseCalendarUniqueId = pointed->uniqueId;
        // Empty week: inherits the base's hours; the MPP writer emits no
        // CALENDAR_DATA blob for it, exactly like a real per-resource row.
        project.calendars.append(cal);
        r.calendarUniqueId = cal.uniqueId;
    }
}

void collapseResourceCalendarPassThroughs(Project &project)
{
    QHash<int, const Calendar *> calByUid;
    QSet<int> baseRefs;
    for (const Calendar &c : project.calendars) {
        calByUid.insert(c.uniqueId, &c);
        if (c.baseCalendarUniqueId >= 0)
            baseRefs.insert(c.baseCalendarUniqueId);
    }
    QHash<int, int> resRefCount;
    for (const Resource &r : project.resources)
        if (r.calendarUniqueId >= 0)
            ++resRefCount[r.calendarUniqueId];
    QSet<int> taskRefs;
    for (const Task &t : project.tasks)
        if (t.calendarUniqueId >= 0)
            taskRefs.insert(t.calendarUniqueId);

    QHash<int, int> collapseTo;   // calendar uid -> its base uid
    for (const Calendar &c : project.calendars) {
        if (c.baseCalendarUniqueId < 0)
            continue;
        const Calendar *base = calByUid.value(c.baseCalendarUniqueId);
        if (!base || base->baseCalendarUniqueId >= 0)
            continue;   // base missing, or itself derived (another resource's)
        if (!customizesNothing(c))
            continue;
        if (resRefCount.value(c.uniqueId) != 1)
            continue;
        if (c.uniqueId == project.calendarUniqueId || taskRefs.contains(c.uniqueId)
                || baseRefs.contains(c.uniqueId))
            continue;
        collapseTo.insert(c.uniqueId, c.baseCalendarUniqueId);
    }
    if (collapseTo.isEmpty())
        return;

    for (Resource &r : project.resources)
        if (collapseTo.contains(r.calendarUniqueId))
            r.calendarUniqueId = collapseTo.value(r.calendarUniqueId);
    QList<Calendar> kept;
    kept.reserve(project.calendars.size());
    for (const Calendar &c : project.calendars)
        if (!collapseTo.contains(c.uniqueId))
            kept.append(c);
    project.calendars = kept;
}

} // namespace schedule
