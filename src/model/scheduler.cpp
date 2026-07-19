// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/scheduler.h"
#include "model/duration.h"
#include "model/workcalendar.h"

#include <QHash>
#include <QMultiHash>
#include <QQueue>
#include <QSet>
#include <QVector>

namespace schedule {

namespace {

// The built-in Standard week backing the static convenience functions.
const WorkCalendar &standardCalendar()
{
    static const WorkCalendar cal;
    return cal;
}

} // namespace

QDateTime Scheduler::nextWorkStart(const QDateTime &dt)
{
    return standardCalendar().nextWorkStart(dt);
}

QDateTime Scheduler::prevWorkEnd(const QDateTime &dt)
{
    return standardCalendar().prevWorkEnd(dt);
}

QDateTime Scheduler::addWork(const QDateTime &from, qint64 millis)
{
    return standardCalendar().addWork(from, millis);
}

qint64 Scheduler::workBetween(const QDateTime &from, const QDateTime &to)
{
    return standardCalendar().workBetween(from, to);
}

WorkCalendar Scheduler::projectCalendar(const Project &project)
{
    int uid = project.calendarUniqueId;
    if (uid < 0) {
        for (const Calendar &c : project.calendars)
            if (c.name == QLatin1String("Standard")) { uid = c.uniqueId; break; }
    }
    return uid >= 0 ? WorkCalendar(project, uid) : WorkCalendar();
}

bool Scheduler::reachable(const Project &project, int fromUid, int toUid)
{
    if (fromUid == toUid)
        return true;
    QMultiHash<int, int> succsOf;
    for (const Relation &rel : project.relations)
        succsOf.insert(rel.predecessorTaskUid, rel.successorTaskUid);
    QSet<int> seen{ fromUid };
    QQueue<int> queue;
    queue.enqueue(fromUid);
    while (!queue.isEmpty()) {
        const int uid = queue.dequeue();
        for (auto it = succsOf.constFind(uid); it != succsOf.constEnd() && it.key() == uid; ++it) {
            if (it.value() == toUid)
                return true;
            if (!seen.contains(it.value())) {
                seen.insert(it.value());
                queue.enqueue(it.value());
            }
        }
    }
    return false;
}

void Scheduler::reschedule(Project &project)
{
    QHash<int, int> idx;   // uid -> tasks index
    idx.reserve(project.tasks.size());
    for (int i = 0; i < project.tasks.size(); ++i)
        idx.insert(project.tasks.at(i).uniqueId, i);

    // Working time is task-calendar first, project calendar otherwise.
    const WorkCalendar projCal = projectCalendar(project);
    QHash<int, WorkCalendar> calByUid;   // calendar uid -> resolved calendar
    auto calendarFor = [&](const Task &t) -> const WorkCalendar & {
        if (t.calendarUniqueId < 0)
            return projCal;
        auto it = calByUid.find(t.calendarUniqueId);
        if (it == calByUid.end())
            it = calByUid.insert(t.calendarUniqueId, WorkCalendar(project, t.calendarUniqueId));
        return it.value();
    };

    // Dependency edges between tasks that exist; self-links are ignored.
    QMultiHash<int, const Relation *> predsOf;   // successor uid -> its relations
    QMultiHash<int, int> succsOf;                // predecessor uid -> successor uids
    QHash<int, int> indegree;
    for (const Task &t : project.tasks)
        indegree.insert(t.uniqueId, 0);
    for (const Relation &rel : project.relations) {
        if (!idx.contains(rel.predecessorTaskUid) || !idx.contains(rel.successorTaskUid)
            || rel.predecessorTaskUid == rel.successorTaskUid)
            continue;
        // An inactive predecessor imposes no constraint: its successors schedule as if
        // the link weren't there (MS Project Inactivate semantics).
        if (!project.tasks.at(idx.value(rel.predecessorTaskUid)).active)
            continue;
        predsOf.insert(rel.successorTaskUid, &rel);
        succsOf.insert(rel.predecessorTaskUid, rel.successorTaskUid);
        ++indegree[rel.successorTaskUid];
    }

    // Kahn topological order; tasks on a dependency cycle never reach the
    // queue and are simply left untouched.
    QQueue<int> queue;
    for (const Task &t : project.tasks)
        if (indegree.value(t.uniqueId) == 0)
            queue.enqueue(t.uniqueId);

    while (!queue.isEmpty()) {
        const int uid = queue.dequeue();
        Task &t = project.tasks[idx.value(uid)];

        // Release successors regardless of whether this task itself moves.
        for (auto it = succsOf.constFind(uid); it != succsOf.constEnd() && it.key() == uid; ++it)
            if (--indegree[it.value()] == 0)
                queue.enqueue(it.value());

        // Summaries roll up from their children (the caller's job); manual tasks,
        // inactive tasks, and tasks that have actually started stay where they are.
        if (t.summary || t.manual || !t.active || t.actualStart.isValid())
            continue;

        const WorkCalendar &cal = calendarFor(t);
        const qint64 dur = qMax<qint64>(0, t.durationMillis);

        // Earliest start allowed by the predecessors.
        QDateTime start;
        bool havePred = false;
        for (auto it = predsOf.constFind(uid); it != predsOf.constEnd() && it.key() == uid; ++it) {
            const Relation *rel = it.value();
            const Task &p = project.tasks.at(idx.value(rel->predecessorTaskUid));

            QDateTime base;
            bool drivesFinish = false;   // FF/SF constrain the successor's finish
            switch (rel->type) {
            case Relation::StartToStart:   base = p.start.isValid() ? p.start : p.finish; break;
            case Relation::FinishToFinish: base = p.finish.isValid() ? p.finish : p.start;
                                           drivesFinish = true; break;
            case Relation::StartToFinish:  base = p.start.isValid() ? p.start : p.finish;
                                           drivesFinish = true; break;
            case Relation::FinishToStart:
            default:                       base = p.finish.isValid() ? p.finish : p.start; break;
            }
            if (!base.isValid())
                continue;

            // Lag: working time normally, wall-clock for elapsed lag units.
            QDateTime point = base;
            if (rel->lagMillis != 0)
                point = Duration::isElapsed(rel->lagFormat) ? base.addMSecs(rel->lagMillis)
                                                            : cal.addWork(base, rel->lagMillis);

            const QDateTime cand = drivesFinish
                ? (dur > 0 ? cal.addWork(point, -dur) : point)
                : cal.nextWorkStart(point);
            if (!cand.isValid())
                continue;
            havePred = true;
            if (!start.isValid() || cand > start)
                start = cand;
        }

        // No predecessors: the task's start anchors it in place. When leveling is
        // active, use the stable un-levelled anchor so its delay doesn't compound.
        if (!havePred)
            start = t.levelingAnchor.isValid() ? t.levelingAnchor : t.start;
        if (!start.isValid())
            continue;

        // Constraints (MSPDI codes): 2 = Must Start On, 3 = Must Finish On,
        // 4 = Start No Earlier Than, 5 = Start No Later Than, 6 = Finish No
        // Earlier Than, 7 = Finish No Later Than. Only ALAP is still treated
        // as ASAP (it needs a backward pass).
        bool fixedByFinish = false;
        if (t.constraintDate.isValid()) {
            switch (t.constraintType) {
            case 2:   // Must Start On
                start = t.constraintDate;
                break;
            case 3:   // Must Finish On
                t.finish = t.constraintDate;
                t.start = dur > 0 ? cal.addWork(t.constraintDate, -dur) : t.constraintDate;
                fixedByFinish = true;
                break;
            case 4:   // Start No Earlier Than
                if (t.constraintDate > start)
                    start = t.constraintDate;
                break;
            case 5:   // Start No Later Than
                // Like MS Project with "honor constraints": the constraint wins
                // over predecessors (which then show negative slack).
                if (start > t.constraintDate)
                    start = t.constraintDate;
                break;
            case 6: { // Finish No Earlier Than
                const QDateTime s = dur > 0 ? cal.addWork(t.constraintDate, -dur)
                                            : t.constraintDate;
                if (s.isValid() && s > start)
                    start = s;
                break;
            }
            case 7: { // Finish No Later Than
                const QDateTime s = dur > 0 ? cal.addWork(t.constraintDate, -dur)
                                            : t.constraintDate;
                if (s.isValid() && start > s)
                    start = s;
                break;
            }
            default:
                break;
            }
        }
        if (fixedByFinish)
            continue;

        // Resource leveling pushes the task later by a working-time delay, on top of
        // whatever predecessors and constraints allow (MS Project's "Leveling Delay").
        if (t.levelingDelayMillis > 0)
            start = cal.addWork(start, t.levelingDelayMillis);

        t.start = start;
        t.finish = dur > 0 ? cal.addWork(start, dur) : start;
    }
}

void Scheduler::computeSlack(Project &project)
{
    QHash<int, int> idx;
    idx.reserve(project.tasks.size());
    for (int i = 0; i < project.tasks.size(); ++i)
        idx.insert(project.tasks.at(i).uniqueId, i);

    const WorkCalendar projCal = projectCalendar(project);
    QHash<int, WorkCalendar> calByUid;
    auto calendarFor = [&](const Task &t) -> const WorkCalendar & {
        if (t.calendarUniqueId < 0)
            return projCal;
        auto it = calByUid.find(t.calendarUniqueId);
        if (it == calByUid.end())
            it = calByUid.insert(t.calendarUniqueId, WorkCalendar(project, t.calendarUniqueId));
        return it.value();
    };

    // Reverse edges: walk successors before their predecessors.
    QMultiHash<int, const Relation *> succLinksOf;   // predecessor uid -> its outgoing links
    QMultiHash<int, int> predsOf;                    // successor uid -> predecessor uids
    QHash<int, int> outdegree;
    for (const Task &t : project.tasks)
        outdegree.insert(t.uniqueId, 0);
    for (const Relation &rel : project.relations) {
        if (!idx.contains(rel.predecessorTaskUid) || !idx.contains(rel.successorTaskUid)
            || rel.predecessorTaskUid == rel.successorTaskUid)
            continue;
        succLinksOf.insert(rel.predecessorTaskUid, &rel);
        predsOf.insert(rel.successorTaskUid, rel.predecessorTaskUid);
        ++outdegree[rel.predecessorTaskUid];
    }

    // The project finish anchors every chain's late finish.
    QDateTime projectFinish;
    for (const Task &t : project.tasks) {
        if (t.summary || !t.finish.isValid())
            continue;
        if (!projectFinish.isValid() || t.finish > projectFinish)
            projectFinish = t.finish;
    }
    if (!projectFinish.isValid())
        return;

    QQueue<int> queue;
    for (const Task &t : project.tasks)
        if (outdegree.value(t.uniqueId) == 0)
            queue.enqueue(t.uniqueId);

    while (!queue.isEmpty()) {
        const int uid = queue.dequeue();
        Task &t = project.tasks[idx.value(uid)];

        // Release predecessors regardless of whether this task participates.
        for (auto it = predsOf.constFind(uid); it != predsOf.constEnd() && it.key() == uid; ++it)
            if (--outdegree[it.value()] == 0)
                queue.enqueue(it.value());

        if (t.summary || !t.start.isValid() || !t.finish.isValid())
            continue;

        const WorkCalendar &cal = calendarFor(t);
        const qint64 dur = qMax<qint64>(0, t.durationMillis);

        // Late finish: the tightest bound the successors impose, else the
        // project finish. SS/SF bound the START; convert via the duration.
        QDateTime lateFinish;
        auto tighten = [&](const QDateTime &cand) {
            if (cand.isValid() && (!lateFinish.isValid() || cand < lateFinish))
                lateFinish = cand;
        };
        for (auto it = succLinksOf.constFind(uid);
             it != succLinksOf.constEnd() && it.key() == uid; ++it) {
            const Relation *rel = it.value();
            const Task &s = project.tasks.at(idx.value(rel->successorTaskUid));
            if (s.summary || !s.lateStart.isValid() || !s.lateFinish.isValid())
                continue;
            QDateTime bound;
            bool boundsStart = false;
            switch (rel->type) {
            case Relation::StartToStart:   bound = s.lateStart;  boundsStart = true; break;
            case Relation::FinishToFinish: bound = s.lateFinish; break;
            case Relation::StartToFinish:  bound = s.lateFinish; boundsStart = true; break;
            case Relation::FinishToStart:
            default:                       bound = s.lateStart;  break;
            }
            if (rel->lagMillis != 0)
                bound = Duration::isElapsed(rel->lagFormat)
                    ? bound.addMSecs(-rel->lagMillis)
                    : cal.addWork(bound, -rel->lagMillis);
            tighten(boundsStart ? (dur > 0 ? cal.addWork(bound, dur) : bound) : bound);
        }
        if (!lateFinish.isValid())
            lateFinish = projectFinish;

        // A deadline or a Must Start/Finish On constraint caps the late dates.
        if (t.deadline.isValid() && t.deadline < lateFinish)
            lateFinish = t.deadline;
        if (t.constraintDate.isValid()) {
            if (t.constraintType == 3 && t.constraintDate < lateFinish)   // Must Finish On
                lateFinish = t.constraintDate;
            else if (t.constraintType == 2) {                             // Must Start On
                const QDateTime f = dur > 0 ? cal.addWork(t.constraintDate, dur)
                                            : t.constraintDate;
                if (f.isValid() && f < lateFinish)
                    lateFinish = f;
            }
        }

        t.lateFinish = lateFinish;
        t.lateStart = dur > 0 ? cal.addWork(lateFinish, -dur) : lateFinish;
        t.totalSlackMillis = t.lateFinish >= t.finish
            ? cal.workBetween(t.finish, t.lateFinish)
            : -cal.workBetween(t.lateFinish, t.finish);
        t.critical = t.totalSlackMillis <= 0;

        // Free slack: how far the task can slip before the EARLIEST successor
        // (early dates) is disturbed; without successors, the project finish.
        QDateTime freeBound;
        auto tightenFree = [&](const QDateTime &cand) {
            if (cand.isValid() && (!freeBound.isValid() || cand < freeBound))
                freeBound = cand;
        };
        for (auto it = succLinksOf.constFind(uid);
             it != succLinksOf.constEnd() && it.key() == uid; ++it) {
            const Relation *rel = it.value();
            const Task &s = project.tasks.at(idx.value(rel->successorTaskUid));
            if (s.summary || !s.start.isValid() || !s.finish.isValid())
                continue;
            QDateTime bound;
            bool boundsStart = false;
            switch (rel->type) {
            case Relation::StartToStart:   bound = s.start;  boundsStart = true; break;
            case Relation::FinishToFinish: bound = s.finish; break;
            case Relation::StartToFinish:  bound = s.finish; boundsStart = true; break;
            case Relation::FinishToStart:
            default:                       bound = s.start;  break;
            }
            if (rel->lagMillis != 0)
                bound = Duration::isElapsed(rel->lagFormat)
                    ? bound.addMSecs(-rel->lagMillis)
                    : cal.addWork(bound, -rel->lagMillis);
            tightenFree(boundsStart ? (dur > 0 ? cal.addWork(bound, dur) : bound) : bound);
        }
        if (!freeBound.isValid())
            freeBound = projectFinish;
        t.freeSlackMillis = qMax<qint64>(0, cal.workBetween(t.finish, freeBound));
    }

    // Summaries: critical when any descendant leaf is; slack is the minimum
    // over the children. Rows are in outline order, so a summary's children
    // follow it until a row at the same or a shallower level.
    for (int i = 0; i < project.tasks.size(); ++i) {
        Task &s = project.tasks[i];
        if (!s.summary)
            continue;
        bool any = false;
        bool critical = false;
        qint64 total = 0, free = 0;
        for (int j = i + 1; j < project.tasks.size(); ++j) {
            const Task &c = project.tasks.at(j);
            if (c.outlineLevel <= s.outlineLevel)
                break;
            if (c.summary)
                continue;
            critical = critical || c.critical;
            if (!any || c.totalSlackMillis < total)
                total = c.totalSlackMillis;
            if (!any || c.freeSlackMillis < free)
                free = c.freeSlackMillis;
            any = true;
        }
        if (any) {
            s.critical = critical;
            s.totalSlackMillis = total;
            s.freeSlackMillis = free;
        }
    }
}

} // namespace schedule
