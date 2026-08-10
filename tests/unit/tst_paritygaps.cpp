// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/customfieldlogic.h"
#include "model/projectreconciliation.h"
#include "model/resourceleveling.h"
#include "model/scheduler.h"
#include "model/taskscheduling.h"
#include "model/workcalendar.h"
#include "mppio.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace schedule;
namespace {
constexpr qint64 kHour = 3600000LL;
Task task(int uid, QDateTime start, qint64 duration) {
    Task value; value.uniqueId=uid; value.id=uid; value.name=QString::number(uid);
    value.start=start; value.durationMillis=duration;
    value.finish=Scheduler::addWork(start,duration); return value;
}
}

class TstParityGaps : public QObject {
    Q_OBJECT
private slots:
    void formulasLookupsAndIndicators();
    void timephasedBaselineAndActualCost();
    void levelingCanSplitRemainingWork();
    void multipleCriticalPathsAndSlackFields();
    void budgetResourcesDoNotInflateTaskTotals();
    void recurringCalendarExceptionsHonorIntervalAndCount();
    void costResourcePreservesEnteredAssignmentCost();
    void scaffoldRoundTripPreservesParityMetadata();
};

void TstParityGaps::formulasLookupsAndIndicators()
{
    Project project; Task value=task(1,QDateTime(QDate(2026,8,3),QTime(8,0)),8*kHour);
    value.cost=1250.0;
    CustomField risk; risk.name="Risk"; risk.value="High"; risk.lookupValues={"Low","High"};
    CustomField score; score.name="Score"; score.formula="IIf([Cost] > 1000, 10, 1)";
    score.graphicalIndicators.append({"ge",10,"warning"});
    value.customFields={risk,score}; project.tasks={value};
    CustomFieldLogic::recalculate(project);
    QCOMPARE(project.tasks.first().customFields.at(1).value.toDouble(),10.0);
    QVERIFY(CustomFieldLogic::acceptsLookupValue(project.tasks.first().customFields.first(),"Low"));
    QVERIFY(!CustomFieldLogic::acceptsLookupValue(project.tasks.first().customFields.first(),"Medium"));
    QCOMPARE(CustomFieldLogic::indicatorFor(project.tasks.first().customFields.at(1)),QString("warning"));
}

void TstParityGaps::timephasedBaselineAndActualCost()
{
    Assignment a; a.uniqueId=10;
    const QDateTime from(QDate(2026,8,3),QTime(8,0)), to(QDate(2026,8,3),QTime(17,0));
    QVERIFY(a.setTimephasedCostInPeriod(TimephasedValue::BaselineCost,from,to,800.0,2));
    QVERIFY(a.setTimephasedCostInPeriod(TimephasedValue::ActualCost,from,to,275.0));
    QCOMPARE(a.timephasedCostInPeriod(TimephasedValue::BaselineCost,from,to,2),800.0);
    QCOMPARE(a.actualCost,275.0);
    QCOMPARE(a.timephasedValues.first().baselineNumber,2);
}

void TstParityGaps::levelingCanSplitRemainingWork()
{
    Project p; Resource r; r.uniqueId=10; r.maxUnits=1.0; p.resources={r};
    const QDateTime monday(QDate(2026,8,3),QTime(8,0));
    Task early=task(1,monday,24*kHour); early.priority=100;
    Task later=task(2,QDateTime(QDate(2026,8,4),QTime(8,0)),8*kHour); later.priority=900;
    p.tasks={early,later};
    Assignment a1; a1.uniqueId=101; a1.taskUniqueId=1; a1.resourceUniqueId=10; a1.units=1; a1.workMillis=24*kHour; a1.remainingWorkMillis=a1.workMillis;
    Assignment a2; a2.uniqueId=102; a2.taskUniqueId=2; a2.resourceUniqueId=10; a2.units=1; a2.workMillis=8*kHour; a2.remainingWorkMillis=a2.workMillis;
    p.assignments={a1,a2}; Scheduler::reschedule(p);
    ResourceLeveling::Options options; options.allowTaskSplitting=true;
    QVERIFY(ResourceLeveling::level(p,options)>0);
    const Task &split=p.tasks.first();
    QVERIFY(split.segments.size()>=2);
    QVERIFY(split.segments.at(1).start>split.segments.at(0).finish);
}

void TstParityGaps::multipleCriticalPathsAndSlackFields()
{
    Project p; const QDateTime monday(QDate(2026,8,3),QTime(8,0));
    p.tasks={task(1,monday,8*kHour),task(2,monday,24*kHour)};
    Scheduler::reschedule(p); Scheduler::computeSlack(p);
    QVERIFY(p.tasks.first().totalSlackMillis>0);
    p.multipleCriticalPaths=true; Scheduler::computeSlack(p);
    QCOMPARE(p.tasks.first().totalSlackMillis,0);
    QCOMPARE(p.tasks.first().startSlackMillis,0);
    QCOMPARE(p.tasks.first().finishSlackMillis,0);
    QCOMPARE(p.tasks.first().earlyStart,p.tasks.first().start);
}

void TstParityGaps::budgetResourcesDoNotInflateTaskTotals()
{
    Project p; p.tasks={task(1,QDateTime(QDate(2026,8,3),QTime(8,0)),8*kHour)};
    Resource r; r.uniqueId=10; r.budget=true; r.budgetCost=50000; p.resources={r};
    Assignment a; a.uniqueId=100; a.taskUniqueId=1; a.resourceUniqueId=10;
    a.budget=true; a.budgetCost=50000; a.budgetWorkMillis=100*kHour; p.assignments={a};
    ProjectReconciliation::reconcile(p);
    QCOMPARE(p.tasks.first().cost,0.0);
    QCOMPARE(p.budgetCost,50000.0);
    QCOMPARE(p.budgetWorkMillis,100*kHour);
}

void TstParityGaps::recurringCalendarExceptionsHonorIntervalAndCount()
{
    Project p; Calendar c; c.uniqueId=1; c.workingDayMask=0x1f;
    CalendarException e; e.fromDate=QDate(2026,8,3); e.toDate=QDate(2026,8,31); e.working=false;
    e.recurrence=CalendarException::Recurrence::Daily; e.interval=2; e.occurrences=3;
    c.exceptions={e}; p.calendars={c}; WorkCalendar calendar(p,1);
    QVERIFY(!calendar.isWorkingDay(QDate(2026,8,3)));
    QVERIFY(!calendar.isWorkingDay(QDate(2026,8,5)));
    QVERIFY(!calendar.isWorkingDay(QDate(2026,8,7)));
    QVERIFY(calendar.isWorkingDay(QDate(2026,8,11)));
}

void TstParityGaps::costResourcePreservesEnteredAssignmentCost()
{
    Project p; p.tasks={task(1,QDateTime(QDate(2026,8,3),QTime(8,0)),8*kHour)};
    Resource r; r.uniqueId=10; r.type=Resource::Type::Cost; p.resources={r};
    Assignment a; a.uniqueId=100; a.taskUniqueId=1; a.resourceUniqueId=10;
    a.remainingCost=725.50; a.cost=725.50; p.assignments={a};
    ProjectReconciliation::reconcile(p);
    QCOMPARE(p.assignments.first().workMillis,0);
    QCOMPARE(p.assignments.first().cost,725.50);
    QCOMPARE(p.tasks.first().cost,725.50);
}

void TstParityGaps::scaffoldRoundTripPreservesParityMetadata()
{
    Project p; p.formatVersion=Project::FormatVersion::Mpp12;
    p.title="Parity metadata"; p.multipleCriticalPaths=true;
    p.budgetCost=12000; p.budgetWorkMillis=40*kHour;
    Task t=task(1,QDateTime(QDate(2026,8,3),QTime(8,0)),8*kHour);
    CustomField f; f.fieldId=1; f.name="Score"; f.value=10.0; f.formula="[Cost]";
    f.lookupValues={"1","10"}; f.graphicalIndicators.append({"ge","10","warning"});
    t.customFields={f}; p.tasks={t};
    Resource r; r.uniqueId=10; r.type=Resource::Type::Cost; r.budget=true;
    r.budgetCost=12000; p.resources={r};
    Assignment a; a.uniqueId=100; a.taskUniqueId=1; a.resourceUniqueId=10;
    a.budget=true; a.budgetCost=12000;
    const QDateTime from(QDate(2026,8,3),QTime(8,0)), to(QDate(2026,8,3),QTime(17,0));
    QVERIFY(a.setTimephasedCostInPeriod(TimephasedValue::BaselineCost,from,to,500.0,3));
    p.assignments={a};
    Calendar c; c.uniqueId=1; c.name="Standard"; c.workingDayMask=0x1f;
    CalendarException e; e.fromDate=QDate(2026,8,3); e.toDate=QDate(2026,8,31);
    e.recurrence=CalendarException::Recurrence::Weekly; e.interval=2;
    e.weekDayMask=1; e.occurrences=2; c.exceptions={e}; p.calendars={c};
    QTemporaryDir dir; QVERIFY(dir.isValid()); const QString path=dir.filePath("parity.mpp");
    MppIO writer; writer.setProject(p); QVERIFY2(writer.save(path),qPrintable(writer.errorString()));
    MppIO reader; QVERIFY2(reader.open(path),qPrintable(reader.errorString()));
    const Project &round=reader.project();
    QVERIFY(round.multipleCriticalPaths); QCOMPARE(round.budgetCost,12000.0);
    QCOMPARE(round.resources.first().type,Resource::Type::Cost); QVERIFY(round.resources.first().budget);
    QVERIFY(round.assignments.first().budget); QCOMPARE(round.assignments.first().budgetCost,12000.0);
    QCOMPARE(round.assignments.first().timephasedValues.first().baselineNumber,3);
    QCOMPARE(round.tasks.first().customFields.first().formula,QString("[Cost]"));
    QCOMPARE(round.tasks.first().customFields.first().graphicalIndicators.first().indicator,QString("warning"));
    QCOMPARE(round.calendars.first().exceptions.first().recurrence,CalendarException::Recurrence::Weekly);
    QCOMPARE(round.calendars.first().exceptions.first().occurrences,2);
}

QTEST_MAIN(TstParityGaps)
#include "tst_paritygaps.moc"
