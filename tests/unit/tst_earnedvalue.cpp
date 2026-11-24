// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/earnedvalue.h"
#include "model/project.h"

#include <QTest>

// Oracle: a three-task schedule baselined in Microsoft Project 2016, status
// date Sun 27 Sep 2026 05:00, rates $44/h. Every expected figure below is what
// Project itself reported for these tasks (Task.BCWS/BCWP/ACWP/SV/CPI via COM,
// 2026-09-27).
class TstEarnedValue : public QObject
{
    Q_OBJECT
private slots:
    void matchesProject();
    void earnsAlongTheBaselineSpanNotTheBudget();
    void noBaselineMeansNoPlannedOrEarnedValue();

private:
    static schedule::Project oracleProject();
};

namespace {

schedule::TimephasedValue baselineCost(int assignment, const QDateTime &start,
                                       const QDateTime &finish, double cost)
{
    schedule::TimephasedValue v;
    v.type = schedule::TimephasedValue::BaselineCost;
    v.uniqueId = assignment;
    v.start = start;
    v.finish = finish;
    v.unit = 1;
    v.value = QString::number(cost, 'g', 15);
    return v;
}

QDateTime at(int month, int day, int hour) { return { QDate(2026, month, day), QTime(hour, 0) }; }

schedule::Baseline baseline(double cost, const QDateTime &start, const QDateTime &finish)
{
    schedule::Baseline b;
    b.number = 0;
    b.cost = cost;
    b.start = start;
    b.finish = finish;
    return b;
}

const schedule::Task &taskByUid(const schedule::Project &p, int uid)
{
    for (const schedule::Task &t : p.tasks)
        if (t.uniqueId == uid)
            return t;
    static const schedule::Task none;
    return none;
}

} // namespace

schedule::Project TstEarnedValue::oracleProject()
{
    schedule::Project p;
    p.statusDate = at(9, 27, 5);

    schedule::Task summary;
    summary.uniqueId = 0; summary.id = 0; summary.outlineLevel = 0; summary.summary = true;
    summary.cost = 15400.0;
    summary.baselines = { baseline(15400.0, at(9, 8, 8), at(10, 19, 15)) };

    // Design: 7 h worked on day one (baseline saved after), then 10 full days.
    schedule::Task design;
    design.uniqueId = 3; design.id = 1; design.outlineLevel = 1;
    design.cost = 3520.0; design.actualCost = 308.0;
    design.percentComplete = 480.0 / 5280.0;
    design.baselines = { baseline(3520.0, at(9, 8, 8), at(9, 22, 17)) };

    // Develop: two resources, 15 days each.
    schedule::Task develop;
    develop.uniqueId = 2; develop.id = 2; develop.outlineLevel = 1;
    develop.cost = 10560.0;
    develop.baselines = { baseline(10560.0, at(9, 23, 8), at(10, 13, 17)) };

    schedule::Task test;
    test.uniqueId = 1; test.id = 3; test.outlineLevel = 1;
    test.cost = 1320.0;
    test.baselines = { baseline(1320.0, at(10, 14, 8), at(10, 19, 15)) };
    p.tasks = { summary, design, develop, test };

    schedule::Assignment a3;
    a3.uniqueId = 3; a3.taskUniqueId = 3; a3.resourceUniqueId = 1;
    a3.timephasedValues = { baselineCost(3, at(9, 8, 8), at(9, 8, 17), 308.0),
                            baselineCost(3, at(9, 9, 8), at(9, 22, 17), 3212.0) };
    schedule::Assignment a1;
    a1.uniqueId = 1; a1.taskUniqueId = 2; a1.resourceUniqueId = 1;
    a1.timephasedValues = { baselineCost(1, at(9, 23, 8), at(10, 13, 17), 5280.0) };
    schedule::Assignment a2 = a1;
    a2.uniqueId = 2; a2.resourceUniqueId = 2;
    schedule::Assignment a4;
    a4.uniqueId = 4; a4.taskUniqueId = 1; a4.resourceUniqueId = 2;
    a4.timephasedValues = { baselineCost(4, at(10, 14, 8), at(10, 19, 15), 1320.0) };
    p.assignments = { a1, a2, a3, a4 };
    return p;
}

void TstEarnedValue::matchesProject()
{
    schedule::Project p = oracleProject();
    schedule::EarnedValueCalculation::recalculate(p);

    const schedule::EarnedValue &design = taskByUid(p, 3).evm;
    QCOMPARE(design.pv, 3520.0);
    QCOMPARE(design.ev, 308.0);
    QCOMPARE(design.ac, 308.0);
    QCOMPARE(design.sv, -3212.0);
    QCOMPARE(design.cpi, 1.0);

    const schedule::EarnedValue &develop = taskByUid(p, 2).evm;
    QCOMPARE(develop.pv, 2112.0);   // Wed-Fri of the baseline, both resources
    QCOMPARE(develop.ev, 0.0);
    QCOMPARE(develop.sv, -2112.0);

    QCOMPARE(taskByUid(p, 1).evm.pv, 0.0);

    const schedule::EarnedValue &project = taskByUid(p, 0).evm;
    QCOMPARE(project.pv, 5632.0);
    QCOMPARE(project.ev, 308.0);
    QCOMPARE(project.ac, 308.0);
    QCOMPARE(project.sv, -5324.0);
    QCOMPARE(project.cpi, 1.0);
}

void TstEarnedValue::earnsAlongTheBaselineSpanNotTheBudget()
{
    // At a stored 9% Project earned $304.92 -- 9% of the 5280-minute span
    // falls inside the $308 first day -- where 9% x BAC would be $316.80.
    schedule::Project p = oracleProject();
    for (schedule::Task &t : p.tasks)
        if (t.uniqueId == 3)
            t.percentComplete = 0.09;
    schedule::EarnedValueCalculation::recalculate(p);
    QCOMPARE(qRound(taskByUid(p, 3).evm.ev * 100.0), 30492);
}

void TstEarnedValue::noBaselineMeansNoPlannedOrEarnedValue()
{
    schedule::Project p = oracleProject();
    for (schedule::Task &t : p.tasks)
        t.baselines.clear();
    for (schedule::Assignment &a : p.assignments)
        a.timephasedValues.clear();
    schedule::EarnedValueCalculation::recalculate(p);
    const schedule::EarnedValue &design = taskByUid(p, 3).evm;
    QCOMPARE(design.pv, 0.0);
    QCOMPARE(design.ev, 0.0);
    QCOMPARE(design.ac, 308.0);
    QCOMPARE(design.cpi, 0.0);
    QCOMPARE(design.eac, 3520.0);   // no CPI yet: the scheduled cost
}

QTEST_MAIN(TstEarnedValue)
#include "tst_earnedvalue.moc"
