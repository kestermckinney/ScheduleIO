// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/resourceleveling.h"
#include "model/scheduler.h"

#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace schedule {

namespace {

constexpr double kEps = 1e-6;

const Task *taskByUid(const Project &p, int uid)
{
    for (const Task &t : p.tasks)
        if (t.uniqueId == uid)
            return &t;
    return nullptr;
}

// The scheduled span an assignment occupies its resource for. We use the task's span
// (a resource is loaded across the task it works on) because it is always current after
// a reschedule -- the per-assignment start/finish are only refreshed by TaskScheduling
// and would go stale as leveling moves tasks. Falls back to the assignment's own dates.
struct Span { QDateTime start; QDateTime finish; };

Span spanOf(const Project &p, const Assignment &a)
{
    if (const Task *t = taskByUid(p, a.taskUniqueId))
        if (t->start.isValid() && t->finish.isValid())
            return { t->start, t->finish };
    return { a.start, a.finish };
}

// Real assignments of a resource (a negative resource uid is the "unassigned" placeholder).
QList<const Assignment *> assignmentsOfResource(const Project &p, int resourceUid)
{
    QList<const Assignment *> out;
    for (const Assignment &a : p.assignments)
        if (a.resourceUniqueId == resourceUid && a.units > kEps)
            out.append(&a);
    return out;
}

double maxUnitsOf(const Project &p, int resourceUid)
{
    for (const Resource &r : p.resources)
        if (r.uniqueId == resourceUid)
            return qMax(r.maxUnits, kEps);
    return 1.0;
}

// Overallocation windows for a single resource (see ResourceLeveling::overallocations).
QList<ResourceLeveling::Overallocation> overForResource(const Project &p, int resourceUid)
{
    const double maxU = maxUnitsOf(p, resourceUid);
    const QList<const Assignment *> assns = assignmentsOfResource(p, resourceUid);

    // Sweep-line: +units when an assignment starts, -units when it finishes. At equal
    // timestamps, apply finishes before starts so touching spans don't count as overlap.
    struct Ev { QDateTime t; double delta; };
    QList<Ev> events;
    for (const Assignment *a : assns) {
        const Span s = spanOf(p, *a);
        if (!s.start.isValid() || !s.finish.isValid() || s.finish <= s.start)
            continue;
        // Use the *effective* units the assignment actually demands -- its work spread
        // over its span -- rather than the stored peak units, so a part-time assignment
        // (a little work over a long span) loads the resource lightly (matching the
        // time-phased hours and MS Project's graph) instead of spuriously reading as a
        // full 100%. The task's duration IS its working-time span, so work/duration is
        // the effective units with no calendar walk.
        double eff = a->units;
        if (const Task *t = taskByUid(p, a->taskUniqueId))
            if (t->durationMillis > 0 && a->workMillis > 0)
                eff = double(a->workMillis) / double(t->durationMillis);
        if (eff <= kEps)
            continue;
        events.append({ s.start, eff });
        events.append({ s.finish, -eff });
    }
    std::sort(events.begin(), events.end(), [](const Ev &x, const Ev &y) {
        if (x.t != y.t) return x.t < y.t;
        return x.delta < y.delta;   // finishes (negative) before starts at the same instant
    });

    QList<ResourceLeveling::Overallocation> out;
    double cur = 0.0;
    bool over = false;
    QDateTime winStart;
    double winPeak = 0.0;
    for (int i = 0; i < events.size(); ) {
        const QDateTime t = events[i].t;
        double d = 0.0;
        while (i < events.size() && events[i].t == t) { d += events[i].delta; ++i; }
        cur += d;

        const bool levelOver = cur > maxU + kEps;
        if (levelOver) {
            if (!over) { over = true; winStart = t; winPeak = cur; }
            else winPeak = qMax(winPeak, cur);
        } else if (over) {
            out.append({ resourceUid, winStart, t, winPeak, maxU });
            over = false;
        }
    }
    if (over && !events.isEmpty())   // shouldn't happen (load returns to 0) but be safe
        out.append({ resourceUid, winStart, events.last().t, winPeak, maxU });
    return out;
}

} // namespace

QList<ResourceLeveling::Overallocation> ResourceLeveling::overallocations(const Project &project)
{
    QList<Overallocation> out;
    for (const Resource &r : project.resources)
        out.append(overForResource(project, r.uniqueId));
    std::sort(out.begin(), out.end(), [](const Overallocation &a, const Overallocation &b) {
        if (a.start != b.start) return a.start < b.start;
        return a.resourceUniqueId < b.resourceUniqueId;
    });
    return out;
}

bool ResourceLeveling::isOverallocated(const Project &project, int resourceUniqueId)
{
    return !overForResource(project, resourceUniqueId).isEmpty();
}

qint64 ResourceLeveling::workInPeriod(const Project &project, const Assignment &assignment,
                                      const QDateTime &from, const QDateTime &to)
{
    const Span s = spanOf(project, assignment);
    if (!s.start.isValid() || !s.finish.isValid() || s.finish <= s.start)
        return 0;
    // Cheap reject first: most grid cells fall outside the assignment's span, so avoid
    // the (calendar-walking) work computations unless the period actually overlaps it.
    const QDateTime a = qMax(s.start, from);
    const QDateTime b = qMin(s.finish, to);
    if (b <= a)
        return 0;
    const qint64 total = Scheduler::workBetween(s.start, s.finish);
    if (total <= 0)
        return 0;
    const qint64 overlap = Scheduler::workBetween(a, b);
    return qint64(std::llround(double(assignment.workMillis) * double(overlap) / double(total)));
}

qint64 ResourceLeveling::resourceWork(const Project &project, int resourceUniqueId)
{
    qint64 total = 0;
    for (const Assignment &a : project.assignments)
        if (a.resourceUniqueId == resourceUniqueId)
            total += a.workMillis;
    return total;
}

int ResourceLeveling::level(Project &project)
{
    // Re-level from a clean slate (like MS Project), then pin each task's current start
    // as its stable anchor so the delays we add below don't compound across reschedules.
    clearLeveling(project);
    for (Task &t : project.tasks)
        t.levelingAnchor = t.start;

    // Horizon: leveling may push work later, but never absurdly so. Cap it at roughly
    // twice the project's span past its finish; a move that would exceed this is left
    // unresolved rather than cascading tasks years out (and blowing up the timeline).
    QDateTime minStart, maxFinish;
    for (const Task &t : project.tasks) {
        if (t.start.isValid() && (!minStart.isValid() || t.start < minStart)) minStart = t.start;
        if (t.finish.isValid() && (!maxFinish.isValid() || t.finish > maxFinish)) maxFinish = t.finish;
    }
    QDateTime horizon;
    if (minStart.isValid() && maxFinish.isValid()) {
        const qint64 spanDays = qMax<qint64>(30, minStart.daysTo(maxFinish));
        horizon = maxFinish.addDays(2 * spanDays);
    }

    QSet<int> delayed;
    QHash<int, int> delayCount;   // per task; caps chasing a single task forever
    constexpr int kMaxPerTask = 40;
    const int maxIterations = 40 * project.tasks.size() + 500;

    // Wall-clock budget: each iteration reschedules the whole project, so a pathological
    // input could otherwise run for many seconds. Ordinary projects converge in a handful
    // of iterations (well under budget); huge overlapped ones stop here and report that
    // some overallocations remain, keeping the UI responsive on any build/machine.
    QElapsedTimer clock;
    clock.start();
    constexpr qint64 kBudgetMs = 1500;

    for (int iter = 0; iter < maxIterations; ++iter) {
        if (clock.elapsed() > kBudgetMs)
            break;
        const QList<Overallocation> over = overallocations(project);
        if (over.isEmpty())
            break;
        const Overallocation &w = over.first();   // earliest window

        // Assignments of this resource whose span overlaps the window, and the movable
        // task behind each (auto-scheduled, not a summary, not already started).
        struct Cand { const Assignment *a; const Task *t; Span span; };
        QList<Cand> active;
        for (const Assignment *a : assignmentsOfResource(project, w.resourceUniqueId)) {
            const Span s = spanOf(project, *a);
            if (!s.start.isValid() || !s.finish.isValid())
                continue;
            if (s.start < w.finish && w.start < s.finish)   // overlaps the window
                active.append({ a, taskByUid(project, a->taskUniqueId), s });
        }
        if (active.size() < 2)
            break;

        // Victim = the lowest-priority, latest-starting movable task; keep the rest. A
        // task delayed too many times is treated as immovable so leveling can't chase a
        // single task forever (which would spin on pathological overlaps).
        auto movable = [&](const Cand &c) {
            return c.t && !c.t->summary && !c.t->manual && !c.t->actualStart.isValid()
                && delayCount.value(c.t->uniqueId) < kMaxPerTask;
        };
        const Cand *victim = nullptr;
        for (const Cand &c : active) {
            if (!movable(c))
                continue;
            if (!victim
                || c.t->priority < victim->t->priority
                || (c.t->priority == victim->t->priority && c.span.start > victim->span.start)
                || (c.t->priority == victim->t->priority && c.span.start == victim->span.start
                    && c.t->uniqueId > victim->t->uniqueId))
                victim = &c;
        }
        if (!victim)
            break;   // nothing in this window can move

        // Free the resource a slot: delay the victim until the soonest OTHER overlapping
        // assignment finishes (that drops concurrency by one across [winStart, that finish]).
        QDateTime freeAt;
        for (const Cand &c : active) {
            if (c.a == victim->a)
                continue;
            if (c.span.start < victim->span.finish && victim->span.start < c.span.finish)
                if (!freeAt.isValid() || c.span.finish < freeAt)
                    freeAt = c.span.finish;
        }
        if (!freeAt.isValid())
            break;
        // Bound the *resulting finish*, not just the new start, so a chain of moves can
        // never cascade tasks past the horizon (which would balloon the timeline).
        if (horizon.isValid()) {
            const QDateTime newFinish = Scheduler::addWork(freeAt, victim->t->durationMillis);
            if (newFinish > horizon)
                break;
        }

        const qint64 add = Scheduler::workBetween(victim->span.start, freeAt);
        if (add <= 0)
            break;   // no forward progress -> stop rather than loop

        for (Task &t : project.tasks)
            if (t.uniqueId == victim->t->uniqueId) {
                t.levelingDelayMillis += add;
                delayed.insert(t.uniqueId);
                delayCount[t.uniqueId] += 1;
                break;
            }
        Scheduler::reschedule(project);
    }

    return delayed.size();
}

void ResourceLeveling::clearLeveling(Project &project)
{
    for (Task &t : project.tasks) {
        // Restore an anchored task's start to its un-levelled position before dropping
        // the anchor (predecessor-driven tasks are recomputed by reschedule regardless).
        if (t.levelingAnchor.isValid())
            t.start = t.levelingAnchor;
        t.levelingAnchor = QDateTime();
        t.levelingDelayMillis = 0;
    }
    Scheduler::reschedule(project);
}

} // namespace schedule
