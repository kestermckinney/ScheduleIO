// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/earnedvalue.h"

#include "model/project.h"
#include "model/scheduler.h"
#include "model/workcalendar.h"

#include <QHash>
#include <algorithm>
#include <cmath>

namespace schedule {

namespace {

// One stretch of the baseline cost spread, with its working-time length.
struct Segment {
    QDateTime start;
    QDateTime finish;
    double cost = 0.0;
    qint64 span = 0;
};

struct Figures {
    double pv = 0.0, ev = 0.0, ac = 0.0;
};

const Baseline *baselineNumbered(const Task &task, int number)
{
    for (const Baseline &b : task.baselines)
        if (b.number == number)
            return &b;
    return nullptr;
}

// The task's baseline cost spread: its assignments' time-phased baseline cost,
// cut at every assignment boundary and summed (what Project keeps on the task).
// Without time-phased values (a fixed-cost task, or a baseline from a source
// that stored none) the baseline cost is spread evenly over the baseline dates,
// else the task's own dates; with no dates at all it can still be earned but
// is never planned by a date.
QList<Segment> costSpread(const QList<TimephasedValue> &values, const Baseline *baseline,
                          const Task &task, const WorkCalendar &calendar)
{
    QList<QDateTime> cuts;
    for (const TimephasedValue &v : values) {
        cuts.append(v.start);
        cuts.append(v.finish);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    QList<Segment> spread;
    for (int i = 0; i + 1 < cuts.size(); ++i) {
        Segment s{ cuts.at(i), cuts.at(i + 1) };
        bool covered = false;
        for (const TimephasedValue &v : values)
            if (v.start < s.finish && v.finish > s.start) {
                covered = true;
                s.cost += v.amountInPeriod(s.start, s.finish);
            }
        if (covered)
            spread.append(s);
    }
    if (spread.isEmpty() && baseline && baseline->cost != 0.0) {
        if (baseline->start.isValid() && baseline->finish > baseline->start)
            spread.append({ baseline->start, baseline->finish, baseline->cost });
        else if (task.start.isValid() && task.finish > task.start)
            spread.append({ task.start, task.finish, baseline->cost });
        else
            spread.append({ QDateTime(), QDateTime(), baseline->cost, 1 });
    }

    for (Segment &s : spread) {
        if (!s.start.isValid())
            continue;
        s.span = calendar.workBetween(s.start, s.finish);
        if (s.span <= 0)
            s.span = s.start.msecsTo(s.finish);
    }
    return spread;
}

// Baseline cost accrued by `status`, pro rata over working time within a segment.
double costThrough(const QList<Segment> &spread, const QDateTime &status,
                   const WorkCalendar &calendar)
{
    double total = 0.0;
    for (const Segment &s : spread) {
        if (!s.start.isValid())
            continue;   // dateless: earned, never planned
        if (s.finish <= status) {
            total += s.cost;
        } else if (s.start < status && s.span > 0) {
            qint64 done = calendar.workBetween(s.start, status);
            if (done <= 0 && calendar.workBetween(s.start, s.finish) <= 0)
                done = s.start.msecsTo(status);   // segment held no working time
            total += s.cost * qBound(0.0, double(done) / double(s.span), 1.0);
        }
    }
    return total;
}

// Baseline cost in the first `fraction` of the spread's working-time span.
double costAtProgress(const QList<Segment> &spread, double fraction)
{
    qint64 totalSpan = 0;
    for (const Segment &s : spread)
        totalSpan += s.span;
    double remaining = qBound(0.0, fraction, 1.0) * double(totalSpan);
    double total = 0.0;
    for (const Segment &s : spread) {
        if (remaining <= 0.0 || s.span <= 0)
            break;
        const double taken = qMin(remaining, double(s.span));
        total += s.cost * taken / double(s.span);
        remaining -= taken;
    }
    return total;
}

void writeDerived(Task &task, const Figures &f, double bac)
{
    EarnedValue &e = task.evm;
    e.pv = f.pv;
    e.ev = f.ev;
    e.ac = f.ac;
    e.sv = f.ev - f.pv;
    e.cv = f.ev - f.ac;
    e.cpi = f.ac != 0.0 ? f.ev / f.ac : 0.0;
    e.spi = f.pv != 0.0 ? f.ev / f.pv : 0.0;
    // EAC = ACWP + (BAC - BCWP) / CPI, which reduces to BAC / CPI; with no
    // CPI yet Project shows the scheduled cost.
    e.eac = e.cpi != 0.0 ? f.ac + (bac - f.ev) / e.cpi : task.cost;
    e.tcpi = (bac - f.ac) != 0.0 ? (bac - f.ev) / (bac - f.ac) : 0.0;
}

} // namespace

QDateTime EarnedValueCalculation::statusDate(const Project &project)
{
    return project.statusDate.isValid()
        ? project.statusDate
        : QDateTime(QDate::currentDate(), project.defaultStartTime);
}

void EarnedValueCalculation::recalculate(Project &project)
{
    const int number = qMax(0, project.baselineForEarnedValue);
    const QDateTime status = statusDate(project);
    const WorkCalendar calendar = Scheduler::projectCalendar(project);

    QHash<int, QList<TimephasedValue>> baselineCost;   // task uid -> values
    for (const Assignment &assignment : std::as_const(project.assignments))
        for (const TimephasedValue &v : assignment.timephasedValues)
            if (v.type == TimephasedValue::BaselineCost && v.baselineNumber == number
                && v.start.isValid() && v.finish > v.start)
                baselineCost[assignment.taskUniqueId].append(v);

    QHash<int, Figures> leafFigures;
    for (Task &task : project.tasks) {
        if (task.summary)
            continue;
        Figures f;
        const Baseline *baseline = baselineNumbered(task, number);
        if (task.active) {
            const QList<Segment> spread =
                costSpread(baselineCost.value(task.uniqueId), baseline, task, calendar);
            f.pv = costThrough(spread, status, calendar);
            f.ev = costAtProgress(spread, task.earnedValueMethod == 1
                                              ? task.physicalPercentComplete
                                              : task.percentComplete);
            f.ac = task.actualCost;
        }
        leafFigures.insert(task.uniqueId, f);
        writeDerived(task, f, baseline ? baseline->cost : 0.0);
    }

    // Summaries sum their active leaves, walking display-ID order like the
    // cost rollup in ProjectReconciliation.
    QList<Task *> ordered;
    ordered.reserve(project.tasks.size());
    for (Task &task : project.tasks)
        ordered.append(&task);
    std::stable_sort(ordered.begin(), ordered.end(), [](const Task *a, const Task *b) {
        return a->id < b->id;
    });
    for (int i = 0; i < ordered.size(); ++i) {
        Task &summary = *ordered.at(i);
        if (!summary.summary)
            continue;
        Figures f;
        for (int j = i + 1; j < ordered.size()
             && ordered.at(j)->outlineLevel > summary.outlineLevel; ++j) {
            const Task &leaf = *ordered.at(j);
            if (leaf.summary)
                continue;
            const Figures l = leafFigures.value(leaf.uniqueId);
            f.pv += l.pv;
            f.ev += l.ev;
            f.ac += l.ac;
        }
        const Baseline *baseline = baselineNumbered(summary, number);
        writeDerived(summary, f, baseline ? baseline->cost : 0.0);
    }
}

} // namespace schedule
