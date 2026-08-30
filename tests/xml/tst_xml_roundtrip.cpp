// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "xmlio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTest>

#include "fixtureutils.h"

// XmlIO must be a faithful reader/writer of the MSPDI model: parsing an XML
// document, writing it back out, and parsing it again must yield an identical
// schedule::Project. This proves the writer emits everything the reader reads (i.e. the
// round trip is lossless for the modelled fields) on real MS Project exports.
class TstXmlRoundtrip : public QObject
{
    Q_OBJECT
private slots:
    void synthetic();
    void fixtures_data();
    void fixtures();
};

void TstXmlRoundtrip::synthetic()
{
    // A small hand-built project exercises every modelled entity without needing
    // any fixture present, so the round trip is always covered.
    schedule::Project p;
    p.formatVersion = schedule::Project::FormatVersion::Mpp14;
    p.title = QStringLiteral("Round & Trip <\"test\">");
    p.author = QStringLiteral("Tester");
    p.startDate = QDateTime(QDate(2026, 1, 5), QTime(8, 0));
    p.finishDate = QDateTime(QDate(2026, 2, 27), QTime(17, 0));
    p.scheduleFromStart = false;
    p.statusDate = QDateTime(QDate(2026, 1, 30), QTime(17, 0));

    // Project options (File > Options) -- every field off its default, so the
    // MSPDI writer/reader pair is proven lossless for them.
    p.newTasksManual = true;
    p.newTaskStartIsProjectStart = false;
    p.defaultTaskType = 2;
    p.defaultDurationUnits = 5;
    p.defaultWorkUnits = 3;
    p.newTasksEffortDriven = true;
    p.autoLinkTasks = false;
    p.splitInProgressTasks = false;
    p.honorConstraints = true;
    p.criticalSlackLimit = 3;
    p.weekStartDay = 1;
    p.fiscalYearStartMonth = 7;
    p.fiscalYearUsesStartYear = true;
    p.defaultStartTime = QTime(9, 30);
    p.defaultEndTime = QTime(18, 15);
    p.minutesPerDay = 450;
    p.minutesPerWeek = 2250;
    p.daysPerMonth = 22;
    p.moveCompletedEndsBack = true;
    p.moveRemainingStartsBack = true;
    p.moveRemainingStartsForward = true;
    p.moveCompletedEndsForward = true;
    p.statusUpdatesResource = false;
    p.currencySymbol = QStringLiteral("kr");
    p.currencySymbolPosition = 3;
    p.currencyDigits = 0;
    p.currencyCode = QStringLiteral("SEK");
    p.defaultStandardRate = 50.0;
    p.defaultOvertimeRate = 75.0;
    p.defaultFixedCostAccrual = 1;
    p.defaultEarnedValueMethod = 1;
    p.baselineForEarnedValue = 3;
    p.showProjectSummaryTask = false;

    schedule::Task summary;
    summary.uniqueId = 1;
    summary.id = 1;
    summary.outlineLevel = 1;
    summary.name = QStringLiteral("Phase 1");
    summary.summary = true;
    summary.recurring = true;
    summary.start = p.startDate;
    summary.finish = p.finishDate;
    summary.durationMillis = 40LL * 3600 * 1000;

    schedule::Task task;
    task.uniqueId = 2;
    task.id = 2;
    task.outlineLevel = 2;
    task.name = QStringLiteral("Design");
    task.start = p.startDate;
    task.finish = QDateTime(QDate(2026, 1, 12), QTime(17, 0));
    task.durationMillis = 32LL * 3600 * 1000;
    task.workMillis = 24LL * 3600 * 1000;
    task.levelingDelayMillis = 2LL * 3600 * 1000;
    task.percentComplete = 0.5;
    task.physicalPercentComplete = 0.35;
    task.earnedValueMethod = 1;
    task.milestone = false;
    task.constraintType = 4;
    task.constraintDate = p.startDate;
    task.manual = true;
    task.effortDriven = true;
    task.taskType = 2;   // Fixed Work
    task.ignoreResourceCalendar = true;
    task.priority = 750;
    task.deadline = QDateTime(QDate(2026, 2, 20), QTime(17, 0));
    task.wbs = QStringLiteral("1.1");
    task.notes = QStringLiteral("{\\rtf1 line one}");
    task.cost = 1234.5;
    task.fixedCost = 100.0;
    task.fixedCostAccrual = 1;   // Start
    task.actualCost = 600.0;
    task.remainingCost = 634.5;
    task.costVariance = 34.5;
    task.startVarianceMillis = 2LL * 3600 * 1000;
    task.finishVarianceMillis = -1LL * 3600 * 1000;
    task.workVarianceMillis = 8LL * 3600 * 1000;
    task.actualStart = task.start;
    task.actualFinish = QDateTime(QDate(2026, 1, 9), QTime(17, 0));
    task.actualDurationMillis = 16LL * 3600 * 1000;
    task.actualWorkMillis = 8LL * 3600 * 1000;
    task.evm.pv = 1000.0;
    task.evm.ev = 617.25;
    task.evm.ac = 600.0;
    task.evm.cv = 17.25;
    task.evm.sv = -382.75;
    task.evm.cpi = 1.02875;
    task.evm.spi = 0.61725;
    task.evm.eac = 1200.0;
    task.evm.tcpi = 0.97;
    task.segments = {
        { task.start, QDateTime(QDate(2026, 1, 7), QTime(12, 0)) },
        { QDateTime(QDate(2026, 1, 8), QTime(8, 0)), task.finish }
    };

    schedule::Baseline base;
    base.number = 0;
    base.cost = 1200.0;
    base.workMillis = 16LL * 3600 * 1000;
    base.start = task.start;
    base.finish = task.finish;
    base.durationMillis = task.durationMillis;
    task.baselines.append(base);

    schedule::CustomField textField;
    textField.fieldId = 0x0B400033;   // task Text1
    textField.name = QStringLiteral("Text1");
    textField.value = QStringLiteral("custom value");
    task.customFields.append(textField);

    p.tasks << summary << task;

    schedule::Relation rel;
    rel.predecessorTaskUid = 1;
    rel.successorTaskUid = 2;
    rel.type = schedule::Relation::FinishToStart;
    rel.lagMillis = 60LL * 60 * 1000;   // 1h, a whole number of tenth-minutes
    p.relations.append(rel);

    schedule::Resource res;
    res.uniqueId = 1;
    res.id = 1;
    res.name = QStringLiteral("Alice");
    res.initials = QStringLiteral("A");
    res.type = schedule::Resource::Type::Material;
    res.materialLabel = QStringLiteral("yards");
    res.maxUnits = 1.0;
    res.cost = 600.0;
    schedule::CostRate cr;
    cr.table = 0;
    cr.startDate = p.startDate;
    cr.standardRate = 75.0;
    cr.standardRateUnit = 2;
    res.costRates.append(cr);
    schedule::AvailabilityPeriod avail;
    avail.startDate = p.startDate;
    avail.endDate = QDateTime(QDate(2026, 1, 31), QTime(23, 59));
    avail.units = 0.5;
    res.availabilityTable.append(avail);
    schedule::AvailabilityPeriod availOpen;   // open-ended row: both dates "NA"
    availOpen.units = 1.0;
    res.availabilityTable.append(availOpen);
    p.resources.append(res);

    schedule::Assignment asn;
    asn.uniqueId = 1;
    asn.taskUniqueId = 2;
    asn.resourceUniqueId = 1;
    asn.units = 1.0;
    asn.costRateTable = 3;
    asn.variableRateUnits = 4;
    asn.workContour = 6;
    asn.workMillis = 16LL * 3600 * 1000;
    asn.overtimeWorkMillis = 3LL * 3600 * 1000;
    asn.actualOvertimeWorkMillis = 2LL * 3600 * 1000;
    asn.remainingOvertimeWorkMillis = 1LL * 3600 * 1000;
    asn.cost = 600.0;
    asn.overtimeCost = 450.0;
    asn.actualOvertimeCost = 300.0;
    asn.remainingOvertimeCost = 150.0;
    schedule::TimephasedValue dayOne;
    dayOne.type = schedule::TimephasedValue::RemainingWork;
    dayOne.uniqueId = 1;
    dayOne.start = QDateTime(QDate(2026, 4, 1), QTime(8, 0));
    dayOne.finish = QDateTime(QDate(2026, 4, 2), QTime(8, 0));
    dayOne.unit = 1;
    dayOne.value = QStringLiteral("PT6H0M0S");
    asn.timephasedValues.append(dayOne);
    schedule::TimephasedValue dayTwo = dayOne;
    dayTwo.uniqueId = 2;
    dayTwo.start = QDateTime(QDate(2026, 4, 2), QTime(8, 0));
    dayTwo.finish = QDateTime(QDate(2026, 4, 3), QTime(8, 0));
    dayTwo.value = QStringLiteral("PT10H0M0S");
    asn.timephasedValues.append(dayTwo);
    schedule::TimephasedValue overtime = dayOne;
    overtime.uniqueId = 3;
    overtime.type = schedule::TimephasedValue::ActualOvertimeWork;
    overtime.value = QStringLiteral("PT2H0M0S");
    asn.timephasedValues.append(overtime);
    p.assignments.append(asn);

    schedule::Calendar cal;
    cal.uniqueId = 1;
    cal.name = QStringLiteral("Standard");
    cal.baseCalendarUniqueId = -1;
    cal.workingDayMask = 0x1F;   // Mon..Fri
    for (int i = 0; i < 7; ++i) {
        QList<schedule::TimeRange> day;
        if (i < 5) {
            day.append({ QTime(8, 0), QTime(12, 0) });
            day.append({ QTime(13, 0), QTime(17, 0) });
        }
        cal.workingTimes.append(day);
    }
    schedule::CalendarException ex;
    ex.fromDate = QDate(2026, 12, 25);
    ex.toDate = QDate(2026, 12, 25);
    ex.name = QStringLiteral("Christmas");
    ex.working = false;
    cal.exceptions.append(ex);
    p.calendars.append(cal);

    XmlIO a;
    a.setProject(p);
    const QByteArray xml = a.saveToData();
    QVERIFY2(!xml.isEmpty(), qPrintable(a.errorString()));

    XmlIO b;
    QVERIFY2(b.openFromData(xml), qPrintable(b.errorString()));

    // The model survives the write->read round trip unchanged...
    QCOMPARE(b.project(), p);

    // ...and a second round trip is stable too.
    XmlIO c;
    QVERIFY(c.openFromData(b.saveToData()));
    QCOMPARE(c.project(), b.project());
}

void TstXmlRoundtrip::fixtures_data()
{
    QTest::addColumn<QString>("xml");
    for (const QString &xml : fixtures::xmlFiles())
        QTest::newRow(qPrintable(fixtures::label(xml))) << xml;
}

void TstXmlRoundtrip::fixtures()
{
    QFETCH(QString, xml);

    XmlIO a;
    QVERIFY2(a.open(xml), qPrintable(a.errorString()));

    const QByteArray written = a.saveToData();
    QVERIFY2(!written.isEmpty(), qPrintable(a.errorString()));

    XmlIO b;
    QVERIFY2(b.openFromData(written), qPrintable(b.errorString()));

    // Real MS Project exports must round-trip through our model losslessly.
    QCOMPARE(b.project(), a.project());
}

QTEST_MAIN(TstXmlRoundtrip)
#include "tst_xml_roundtrip.moc"
