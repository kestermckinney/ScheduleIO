// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/resourceleveling.h"
#include "model/schedulingcalendar.h"
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
    if (const Task *t = taskByUid(p, a.taskUniqueId)) {
        if (t->start.isValid()) {
            const WorkCalendar calendar = SchedulingCalendar::assignment(p, *t, a);
            QDateTime start = calendar.nextWorkStart(t->start);
            const qint64 delay = a.delayMillis + a.levelingDelayMillis;
            if (delay != 0)
                start = calendar.addWork(start, delay);
            const qint64 workSpan = a.workMillis > 0
                ? qint64(std::llround(double(a.workMillis) / qMax(a.units, kEps)))
                : 0;
            return { start, workSpan > 0 ? calendar.addWork(start, workSpan) : start };
        }
    }
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

const Resource *resourceByUid(const Project &p, int resourceUid)
{
    for (const Resource &resource : p.resources)
        if (resource.uniqueId == resourceUid)
            return &resource;
    return nullptr;
}

QDateTime availabilityEndExclusive(const AvailabilityPeriod &period)
{
    if (!period.endDate.isValid())
        return {};
    // The Resource Sheet editor stores date-only Available To values at
    // midnight. Microsoft treats that date as included, so its boundary is
    // the start of the following day.
    return period.endDate.time() == QTime(0, 0)
        ? period.endDate.addDays(1) : period.endDate;
}

double availableUnitsAt(const Resource &resource, const QDateTime &when)
{
    if (resource.availabilityTable.isEmpty())
        return qMax(0.0, resource.maxUnits);

    const AvailabilityPeriod *best = nullptr;
    for (const AvailabilityPeriod &period : resource.availabilityTable) {
        const QDateTime end = availabilityEndExclusive(period);
        if (period.startDate.isValid() && when < period.startDate)
            continue;
        if (end.isValid() && when >= end)
            continue;
        if (!best || (!best->startDate.isValid() && period.startDate.isValid())
            || (period.startDate.isValid() && period.startDate > best->startDate))
            best = &period;
    }
    // Project permits assignments outside the availability table, but marks
    // the resource overallocated there; zero capacity models that behavior.
    return best ? qMax(0.0, best->units) : 0.0;
}

// Overallocation windows for a single resource (see ResourceLeveling::overallocations).
QList<ResourceLeveling::Overallocation> overForResource(const Project &p, int resourceUid)
{
    const Resource *resource = resourceByUid(p, resourceUid);
    if (!resource || resource->type != Resource::Type::Work)
        return {};
    const QList<const Assignment *> assns = assignmentsOfResource(p, resourceUid);

    struct Load { QDateTime start; QDateTime finish; double units = 0.0; };
    QList<Load> loads;
    QList<QDateTime> points;
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
        loads.append({ s.start, s.finish, eff });
        points.append(s.start);
        points.append(s.finish);
    }
    for (const AvailabilityPeriod &period : resource->availabilityTable) {
        if (period.startDate.isValid())
            points.append(period.startDate);
        const QDateTime end = availabilityEndExclusive(period);
        if (end.isValid())
            points.append(end);
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());

    QList<ResourceLeveling::Overallocation> out;
    for (int i = 0; i + 1 < points.size(); ++i) {
        const QDateTime from = points.at(i);
        const QDateTime to = points.at(i + 1);
        if (to <= from)
            continue;
        double load = 0.0;
        for (const Load &item : loads)
            if (item.start <= from && from < item.finish)
                load += item.units;
        const double capacity = availableUnitsAt(*resource, from);
        if (load <= capacity + kEps)
            continue;
        if (!out.isEmpty() && out.last().finish == from
            && qAbs(out.last().maxUnits - capacity) <= kEps) {
            out.last().finish = to;
            out.last().peakUnits = qMax(out.last().peakUnits, load);
        } else {
            out.append({ resourceUid, from, to, load, capacity });
        }
    }
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

ResourceLeveling::WorkProfile ResourceLeveling::workProfile(const Project &project,
                                                            const Assignment &assignment)
{
    WorkProfile profile;
    profile.assignment = &assignment;
    for (const TimephasedValue &v : assignment.timephasedValues) {
        if (v.type == TimephasedValue::RemainingWork
            || v.type == TimephasedValue::ActualWork) {
            profile.timephased = true;
            break;
        }
    }
    if (profile.timephased)
        return profile;

    const Span s = spanOf(project, assignment);
    if (!s.start.isValid() || !s.finish.isValid() || s.finish <= s.start)
        return profile;
    profile.start = s.start;
    profile.finish = s.finish;
    const Task *task = taskByUid(project, assignment.taskUniqueId);
    profile.calendar = task
        ? SchedulingCalendar::assignment(project, *task, assignment)
        : Scheduler::projectCalendar(project);
    profile.spanWork = profile.calendar.workBetween(s.start, s.finish);

    // The two ids that calendar was derived from -- kept in step with
    // SchedulingCalendar::assignment(), which is what built it above.
    if (task) {
        profile.taskCalendarUid = task->calendarUniqueId;
        const Resource *resource = resourceByUid(project, assignment.resourceUniqueId);
        if (!task->manual && !task->ignoreResourceCalendar && resource
            && resource->type == Resource::Type::Work)
            profile.resourceCalendarUid = resource->calendarUniqueId;
    }
    return profile;
}

qint64 ResourceLeveling::workInPeriod(const WorkProfile &profile,
                                      const QDateTime &from, const QDateTime &to)
{
    if (!profile.assignment)
        return 0;
    if (profile.timephased) {
        qint64 timephased = 0;
        for (const TimephasedValue &v : profile.assignment->timephasedValues)
            if (v.type == TimephasedValue::RemainingWork
                || v.type == TimephasedValue::ActualWork)
                timephased += v.durationInPeriod(from, to);
        return timephased;
    }
    if (!profile.start.isValid() || !profile.finish.isValid() || profile.spanWork <= 0)
        return 0;
    // Cheap reject first: most grid cells fall outside the assignment's span, so avoid
    // the (calendar-walking) work computations unless the period actually overlaps it.
    const QDateTime a = qMax(profile.start, from);
    const QDateTime b = qMin(profile.finish, to);
    if (b <= a)
        return 0;
    const qint64 overlap = profile.calendar.workBetween(a, b);
    return qint64(std::llround(double(profile.assignment->workMillis) * double(overlap)
                               / double(profile.spanWork)));
}

qint64 ResourceLeveling::workInPeriod(const Project &project, const Assignment &assignment,
                                      const QDateTime &from, const QDateTime &to)
{
    return workInPeriod(workProfile(project, assignment), from, to);
}

qint64 ResourceLeveling::resourceWork(const Project &project, int resourceUniqueId)
{
    qint64 total = 0;
    for (const Assignment &a : project.assignments)
        if (a.resourceUniqueId == resourceUniqueId)
            total += a.workMillis;
    return total;
}

namespace {
bool splitForLeveling(Project &project, int taskUid, const QDateTime &split,
                      const QDateTime &resume)
{
    Task *task = nullptr;
    for (Task &candidate : project.tasks) if (candidate.uniqueId == taskUid) { task = &candidate; break; }
    if (!task || task->summary || task->milestone || resume <= split
        || split <= task->start || split >= task->finish) return false;
    QList<TaskSegment> portions = task->segments;
    if (portions.size() < 2) portions = {{task->start, task->finish}};
    int part = -1;
    for (int i=0;i<portions.size();++i) if (split > portions[i].start && split < portions[i].finish) { part=i; break; }
    if (part < 0) return false;
    const qint64 shift = split.msecsTo(resume);
    const QDateTime oldFinish = portions[part].finish;
    portions[part].finish = split;
    portions.insert(part+1, {resume, oldFinish.addMSecs(shift)});
    for (int i=part+2;i<portions.size();++i) { portions[i].start=portions[i].start.addMSecs(shift); portions[i].finish=portions[i].finish.addMSecs(shift); }
    task->segments = portions; task->finish = portions.last().finish;
    for (Assignment &assignment : project.assignments) if (assignment.taskUniqueId == taskUid) {
        assignment.finish = task->finish;
        if (assignment.actualWorkMillis != 0 || assignment.workMillis <= 0) continue;
        for (int i=assignment.timephasedValues.size()-1;i>=0;--i)
            if (assignment.timephasedValues[i].type == TimephasedValue::RemainingWork)
                assignment.timephasedValues.removeAt(i);
        qint64 span=0, assigned=0;
        for (const TaskSegment &s : portions) span += qMax<qint64>(0,s.start.msecsTo(s.finish));
        for (int i=0;i<portions.size();++i) {
            const qint64 amount = i+1==portions.size() ? assignment.workMillis-assigned
                : qRound64(double(assignment.workMillis)*double(portions[i].start.msecsTo(portions[i].finish))/double(qMax<qint64>(1,span)));
            assignment.setTimephasedWorkInPeriod(TimephasedValue::RemainingWork,portions[i].start,portions[i].finish,amount);
            assigned += amount;
        }
        assignment.workContour = 8;
    }
    return true;
}
}

int ResourceLeveling::level(Project &project)
{
    return level(project, Options{});
}

int ResourceLeveling::level(Project &project, const Options &options)
{
    // Re-level from a clean slate (like MS Project), then pin each task's current start
    // as its stable anchor so the delays we add below don't compound across reschedules.
    clearLeveling(project);
    Scheduler::computeSlack(project);
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
    QSet<QString> blockedWindows;
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
        const Overallocation *window = nullptr;
        QString windowKey;
        for (const Overallocation &candidate : over) {
            const QString key = QStringLiteral("%1:%2:%3")
                .arg(candidate.resourceUniqueId)
                .arg(candidate.start.toMSecsSinceEpoch())
                .arg(candidate.finish.toMSecsSinceEpoch());
            if (!blockedWindows.contains(key)) {
                window = &candidate;
                windowKey = key;
                break;
            }
        }
        if (!window)
            break;
        const Overallocation &w = *window;   // earliest not-yet-blocked window

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
        if (active.isEmpty()) {
            blockedWindows.insert(windowKey);
            continue;
        }

        // Victim = the lowest-priority, latest-starting movable task; keep the rest. A
        // task delayed too many times is treated as immovable so leveling can't chase a
        // single task forever (which would spin on pathological overlaps).
        auto movable = [&](const Cand &c) {
            return c.t && c.t->active && !c.t->summary && !c.t->manual
                && !c.t->actualStart.isValid()
                && delayCount.value(c.t->uniqueId) < kMaxPerTask;
        };
        QList<const Cand *> candidates;
        for (const Cand &c : active) {
            if (!movable(c))
                continue;
            if (!options.taskUniqueIds.isEmpty()
                && !options.taskUniqueIds.contains(c.t->uniqueId))
                continue;
            candidates.append(&c);
        }
        auto standardLess = [](const Cand *a, const Cand *b) {
            if (a->t->totalSlackMillis != b->t->totalSlackMillis)
                return a->t->totalSlackMillis > b->t->totalSlackMillis;
            if (a->span.start != b->span.start)
                return a->span.start > b->span.start;
            return a->t->uniqueId > b->t->uniqueId;
        };
        std::sort(candidates.begin(), candidates.end(), [&](const Cand *a, const Cand *b) {
            if (options.order == Order::IdOnly)
                return a->t->id != b->t->id
                    ? a->t->id > b->t->id : a->t->uniqueId > b->t->uniqueId;
            if (options.order == Order::PriorityStandard
                && a->t->priority != b->t->priority)
                return a->t->priority < b->t->priority;
            return standardLess(a, b);
        });
        if (candidates.isEmpty()) {
            blockedWindows.insert(windowKey);
            continue;   // nothing in this window can move; try another conflict
        }

        // Try candidates in the selected leveling order. With the within-slack option,
        // a candidate that cannot move far enough without making its slack negative is
        // skipped so another eligible task in the same conflict can still be considered.
        const Cand *victim = nullptr;
        QDateTime freeAt;
        qint64 add = 0;
        for (const Cand *candidate : candidates) {
            QDateTime candidateFreeAt;
            for (const Cand &c : active) {
                if (c.a == candidate->a)
                    continue;
                if (c.span.start < candidate->span.finish
                    && candidate->span.start < c.span.finish)
                    if (!candidateFreeAt.isValid() || c.span.finish < candidateFreeAt)
                        candidateFreeAt = c.span.finish;
            }
            if (!candidateFreeAt.isValid() && active.size() == 1)
                candidateFreeAt = w.finish;
            if (!candidateFreeAt.isValid())
                continue;

            const WorkCalendar calendar = SchedulingCalendar::taskBase(project, *candidate->t);
            const qint64 candidateAdd = calendar.workBetween(candidate->span.start,
                                                              candidateFreeAt);
            if (candidateAdd <= 0)
                continue;
            if (options.levelOnlyWithinAvailableSlack
                && candidateAdd > qMax<qint64>(0, candidate->t->totalSlackMillis))
                continue;
            victim = candidate;
            freeAt = candidateFreeAt;
            add = candidateAdd;
            break;
        }
        if (!victim) {
            blockedWindows.insert(windowKey);
            continue;
        }
        // Bound the *resulting finish*, not just the new start, so a chain of moves can
        // never cascade tasks past the horizon (which would balloon the timeline).
        if (horizon.isValid()) {
            const QDateTime newFinish = SchedulingCalendar::finish(project, *victim->t, freeAt);
            if (newFinish > horizon) {
                blockedWindows.insert(windowKey);
                continue;
            }
        }

        if (options.allowTaskSplitting && w.start > victim->span.start
            && w.start < victim->span.finish && freeAt > w.start
            && splitForLeveling(project, victim->t->uniqueId, w.start, freeAt)) {
            delayed.insert(victim->t->uniqueId);
            delayCount[victim->t->uniqueId] += 1;
            Scheduler::reschedule(project);
            Scheduler::computeSlack(project);
            blockedWindows.clear();
            continue;
        }

        for (Task &t : project.tasks)
            if (t.uniqueId == victim->t->uniqueId) {
                t.levelingDelayMillis += add;
                delayed.insert(t.uniqueId);
                delayCount[t.uniqueId] += 1;
                break;
        }
        Scheduler::reschedule(project);
        Scheduler::computeSlack(project);
        // Moving a task can change or eliminate any previously blocked window.
        blockedWindows.clear();
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
