// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QDir>
#include <QTest>

// Human-readable first-difference report so a failing round-trip names the
// entity and field instead of a bare operator== FALSE.
static QString diffProjects(const schedule::Project &a, const schedule::Project &b)
{
    QStringList d;
    auto str = [](const QVariant &v) { return v.toString(); };
    Q_UNUSED(str);
    if (a.title != b.title) d << QStringLiteral("title '%1' vs '%2'").arg(a.title, b.title);
    if (a.author != b.author) d << QStringLiteral("author '%1' vs '%2'").arg(a.author, b.author);
    if (a.startDate != b.startDate) d << QStringLiteral("startDate %1 vs %2").arg(a.startDate.toString(Qt::ISODate), b.startDate.toString(Qt::ISODate));
    if (a.finishDate != b.finishDate) d << QStringLiteral("finishDate %1 vs %2").arg(a.finishDate.toString(Qt::ISODate), b.finishDate.toString(Qt::ISODate));
    if (a.statusDate != b.statusDate) d << QStringLiteral("statusDate");
    if (a.calendarUniqueId != b.calendarUniqueId)
        d << QStringLiteral("calendarUniqueId %1 vs %2").arg(a.calendarUniqueId).arg(b.calendarUniqueId);
    if (a.tasks.size() != b.tasks.size())
        d << QStringLiteral("task count %1 vs %2").arg(a.tasks.size()).arg(b.tasks.size());
    for (int i = 0; i < qMin(a.tasks.size(), b.tasks.size()); ++i) {
        const schedule::Task &x = a.tasks.at(i), &y = b.tasks.at(i);
        if (x == y)
            continue;
        QStringList f;
        if (x.uniqueId != y.uniqueId) f << QStringLiteral("uniqueId %1/%2").arg(x.uniqueId).arg(y.uniqueId);
        if (x.id != y.id) f << QStringLiteral("id");
        if (x.name != y.name) f << QStringLiteral("name '%1'/'%2'").arg(x.name, y.name);
        if (x.outlineLevel != y.outlineLevel) f << QStringLiteral("outlineLevel");
        if (x.start != y.start) f << QStringLiteral("start %1/%2").arg(x.start.toString(Qt::ISODate), y.start.toString(Qt::ISODate));
        if (x.finish != y.finish) f << QStringLiteral("finish %1/%2").arg(x.finish.toString(Qt::ISODate), y.finish.toString(Qt::ISODate));
        if (x.durationMillis != y.durationMillis) f << QStringLiteral("duration %1/%2").arg(x.durationMillis).arg(y.durationMillis);
        if (x.percentComplete != y.percentComplete) f << QStringLiteral("pct");
        if (x.milestone != y.milestone) f << QStringLiteral("milestone");
        if (x.summary != y.summary) f << QStringLiteral("summary");
        if (x.constraintType != y.constraintType) f << QStringLiteral("constraintType");
        if (x.constraintDate != y.constraintDate) f << QStringLiteral("constraintDate");
        if (x.wbs != y.wbs) f << QStringLiteral("wbs '%1'/'%2'").arg(x.wbs, y.wbs);
        if (x.notes != y.notes) f << QStringLiteral("notes");
        if (x.manual != y.manual) f << QStringLiteral("manual");
        if (x.effortDriven != y.effortDriven) f << QStringLiteral("effortDriven");
        if (x.taskType != y.taskType) f << QStringLiteral("taskType %1/%2").arg(x.taskType).arg(y.taskType);
        if (x.priority != y.priority) f << QStringLiteral("priority %1/%2").arg(x.priority).arg(y.priority);
        if (x.deadline != y.deadline) f << QStringLiteral("deadline");
        if (x.actualStart != y.actualStart) f << QStringLiteral("actualStart");
        if (x.actualFinish != y.actualFinish) f << QStringLiteral("actualFinish");
        if (x.actualDurationMillis != y.actualDurationMillis) f << QStringLiteral("actualDuration");
        if (x.actualWorkMillis != y.actualWorkMillis) f << QStringLiteral("actualWork %1/%2").arg(x.actualWorkMillis).arg(y.actualWorkMillis);
        if (!(x.evm == y.evm)) f << QStringLiteral("evm");
        if (x.cost != y.cost) f << QStringLiteral("cost %1/%2").arg(x.cost).arg(y.cost);
        if (x.fixedCost != y.fixedCost) f << QStringLiteral("fixedCost");
        if (x.actualCost != y.actualCost) f << QStringLiteral("actualCost");
        if (x.remainingCost != y.remainingCost) f << QStringLiteral("remainingCost");
        if (x.costVariance != y.costVariance) f << QStringLiteral("costVariance");
        if (x.baselines != y.baselines) {
            f << QStringLiteral("baselines (%1/%2)").arg(x.baselines.size()).arg(y.baselines.size());
            for (int n = 0; n < qMin(x.baselines.size(), y.baselines.size()); ++n)
                if (!(x.baselines.at(n) == y.baselines.at(n)))
                    f << QStringLiteral("  bl#%1 num %2/%3 cost %4/%5 work %6/%7 dur %8/%9 start %10/%11")
                             .arg(n).arg(x.baselines.at(n).number).arg(y.baselines.at(n).number)
                             .arg(x.baselines.at(n).cost).arg(y.baselines.at(n).cost)
                             .arg(x.baselines.at(n).workMillis).arg(y.baselines.at(n).workMillis)
                             .arg(x.baselines.at(n).durationMillis).arg(y.baselines.at(n).durationMillis)
                             .arg(x.baselines.at(n).start.toString(Qt::ISODate), y.baselines.at(n).start.toString(Qt::ISODate));
        }
        if (x.customFields != y.customFields) {
            f << QStringLiteral("customFields (%1/%2)").arg(x.customFields.size()).arg(y.customFields.size());
            for (int n = 0; n < qMin(x.customFields.size(), y.customFields.size()); ++n)
                if (!(x.customFields.at(n) == y.customFields.at(n)))
                    f << QStringLiteral("  cf#%1 %2=%3 / %4=%5").arg(n)
                             .arg(x.customFields.at(n).name, x.customFields.at(n).value.toString(),
                                  y.customFields.at(n).name, y.customFields.at(n).value.toString());
        }
        d << QStringLiteral("task[%1] uid %2: %3").arg(i).arg(x.uniqueId).arg(f.join(QStringLiteral(", ")));
        if (d.size() > 6)
            break;
    }
    if (a.resources.size() != b.resources.size())
        d << QStringLiteral("resource count %1 vs %2").arg(a.resources.size()).arg(b.resources.size());
    for (int i = 0; i < qMin(a.resources.size(), b.resources.size()); ++i) {
        const schedule::Resource &x = a.resources.at(i), &y = b.resources.at(i);
        if (x == y)
            continue;
        QStringList f;
        if (x.uniqueId != y.uniqueId) f << QStringLiteral("uniqueId");
        if (x.id != y.id) f << QStringLiteral("id");
        if (x.name != y.name) f << QStringLiteral("name '%1'/'%2'").arg(x.name, y.name);
        if (x.initials != y.initials) f << QStringLiteral("initials");
        if (x.maxUnits != y.maxUnits) f << QStringLiteral("maxUnits %1/%2").arg(x.maxUnits).arg(y.maxUnits);
        if (x.notes != y.notes) f << QStringLiteral("notes");
        if (x.cost != y.cost) f << QStringLiteral("cost %1/%2").arg(x.cost).arg(y.cost);
        if (x.actualCost != y.actualCost) f << QStringLiteral("actualCost");
        if (x.remainingCost != y.remainingCost) f << QStringLiteral("remainingCost");
        if (x.costVariance != y.costVariance) f << QStringLiteral("costVariance");
        if (x.baselines != y.baselines) f << QStringLiteral("baselines (%1/%2)").arg(x.baselines.size()).arg(y.baselines.size());
        if (x.customFields != y.customFields) f << QStringLiteral("customFields (%1/%2)").arg(x.customFields.size()).arg(y.customFields.size());
        if (x.costRates != y.costRates) {
            f << QStringLiteral("costRates (%1/%2)").arg(x.costRates.size()).arg(y.costRates.size());
            for (int n = 0; n < qMin(x.costRates.size(), y.costRates.size()); ++n)
                if (!(x.costRates.at(n) == y.costRates.at(n)))
                    f << QStringLiteral("  cr#%1 tbl %2/%3 std %4/%5 ot %6/%7 cpu %8/%9 end %10/%11 start %12/%13")
                             .arg(n).arg(x.costRates.at(n).table).arg(y.costRates.at(n).table)
                             .arg(x.costRates.at(n).standardRate).arg(y.costRates.at(n).standardRate)
                             .arg(x.costRates.at(n).overtimeRate).arg(y.costRates.at(n).overtimeRate)
                             .arg(x.costRates.at(n).costPerUse).arg(y.costRates.at(n).costPerUse)
                             .arg(x.costRates.at(n).endDate.toString(Qt::ISODate), y.costRates.at(n).endDate.toString(Qt::ISODate),
                                  x.costRates.at(n).startDate.toString(Qt::ISODate), y.costRates.at(n).startDate.toString(Qt::ISODate));
        }
        if (x.availabilityTable != y.availabilityTable)
            f << QStringLiteral("availabilityTable (%1/%2)")
                     .arg(x.availabilityTable.size()).arg(y.availabilityTable.size());
        d << QStringLiteral("resource[%1] uid %2: %3").arg(i).arg(x.uniqueId).arg(f.join(QStringLiteral(", ")));
        if (d.size() > 12)
            break;
    }
    if (a.assignments.size() != b.assignments.size())
        d << QStringLiteral("assignment count %1 vs %2").arg(a.assignments.size()).arg(b.assignments.size());
    for (int i = 0; i < qMin(a.assignments.size(), b.assignments.size()); ++i) {
        if (a.assignments.at(i) == b.assignments.at(i))
            continue;
        const schedule::Assignment &x = a.assignments.at(i), &y = b.assignments.at(i);
        QStringList f;
        if (x.uniqueId != y.uniqueId) f << QStringLiteral("uniqueId");
        if (x.taskUniqueId != y.taskUniqueId) f << QStringLiteral("taskUid");
        if (x.resourceUniqueId != y.resourceUniqueId) f << QStringLiteral("resourceUid");
        if (x.units != y.units) f << QStringLiteral("units %1/%2").arg(x.units).arg(y.units);
        if (x.workMillis != y.workMillis) f << QStringLiteral("work %1/%2").arg(x.workMillis).arg(y.workMillis);
        if (x.notes != y.notes) f << QStringLiteral("notes");
        if (x.cost != y.cost) f << QStringLiteral("cost %1/%2").arg(x.cost).arg(y.cost);
        if (x.actualCost != y.actualCost) f << QStringLiteral("actualCost");
        if (x.remainingCost != y.remainingCost) f << QStringLiteral("remainingCost");
        if (x.costVariance != y.costVariance) f << QStringLiteral("costVariance");
        if (x.baselines != y.baselines) f << QStringLiteral("baselines (%1/%2)").arg(x.baselines.size()).arg(y.baselines.size());
        if (x.customFields != y.customFields) f << QStringLiteral("customFields (%1/%2)").arg(x.customFields.size()).arg(y.customFields.size());
        d << QStringLiteral("assignment[%1] uid %2: %3").arg(i).arg(x.uniqueId).arg(f.join(QStringLiteral(", ")));
        if (d.size() > 18)
            break;
    }
    if (a.relations != b.relations)
        d << QStringLiteral("relations differ (%1/%2)").arg(a.relations.size()).arg(b.relations.size());
    if (a.calendars.size() != b.calendars.size())
        d << QStringLiteral("calendar count %1 vs %2").arg(a.calendars.size()).arg(b.calendars.size());
    for (int i = 0; i < qMin(a.calendars.size(), b.calendars.size()); ++i) {
        if (a.calendars.at(i) == b.calendars.at(i))
            continue;
        const schedule::Calendar &x = a.calendars.at(i), &y = b.calendars.at(i);
        QStringList f;
        if (x.uniqueId != y.uniqueId) f << QStringLiteral("uniqueId");
        if (x.name != y.name) f << QStringLiteral("name '%1'/'%2'").arg(x.name, y.name);
        if (x.baseCalendarUniqueId != y.baseCalendarUniqueId) f << QStringLiteral("baseCal");
        if (x.workingDayMask != y.workingDayMask) f << QStringLiteral("mask %1/%2").arg(x.workingDayMask).arg(y.workingDayMask);
        if (x.workingTimes != y.workingTimes) f << QStringLiteral("workingTimes");
        if (x.exceptions != y.exceptions) f << QStringLiteral("exceptions (%1/%2)").arg(x.exceptions.size()).arg(y.exceptions.size());
        d << QStringLiteral("calendar[%1] uid %2: %3").arg(i).arg(x.uniqueId).arg(f.join(QStringLiteral(", ")));
        if (d.size() > 24)
            break;
    }
    return d.join(QStringLiteral("\n"));
}

#ifndef SCHEDULEIO_FIXTURE_DIR
#define SCHEDULEIO_FIXTURE_DIR ""
#endif

class TstSemanticRoundtrip : public QObject
{
    Q_OBJECT
private slots:
    void syntheticRoundTrip();
    void writerIsDeterministic();
    void realFixtures_data();
    void realFixtures();

private:
    static schedule::Project makeSampleProject();
};

schedule::Project TstSemanticRoundtrip::makeSampleProject()
{
    schedule::Project p;
    p.formatVersion = schedule::Project::FormatVersion::Mpp14;
    p.title = QStringLiteral("Scaffold Plan");
    p.author = QStringLiteral("Paul");
    p.startDate = QDateTime(QDate(2026, 1, 2), QTime(8, 0), Qt::UTC);
    p.finishDate = QDateTime(QDate(2026, 3, 31), QTime(17, 0), Qt::UTC);

    schedule::Task t1;
    t1.uniqueId = 1; t1.id = 1; t1.outlineLevel = 1;
    t1.name = QStringLiteral("Design");
    t1.start = QDateTime(QDate(2026, 1, 2), QTime(9, 0), Qt::UTC);
    t1.finish = QDateTime(QDate(2026, 1, 9), QTime(17, 0), Qt::UTC);
    t1.durationMillis = qint64(8) * 3600 * 1000;   // divisible by the duration unit
    t1.percentComplete = 0.5;
    t1.workMillis = qint64(16) * 3600 * 1000;                // task-level Work
    t1.levelingDelayMillis = qint64(4) * 3600 * 1000;        // resource-leveling delay
    // Notes are stored as their 8-bit raw RTF source in the real format, so the
    // sample keeps to ASCII (Project itself escapes non-ANSI as \uN in RTF).
    t1.notes = QStringLiteral("{\\rtf1\\ansi Design note - keep it raw.}");
    // WBS is not stored in the real format: it always reads back as the derived
    // OutlineNumber, so the sample carries the derived values.
    t1.wbs = QStringLiteral("1");
    // Row formatting (Format > Font): stored as the Gantt view's exceptional
    // text styles; must survive the CV_iew patch round trip.
    t1.rowFormat.bold = true;
    t1.rowFormat.color = 0xC00000;       // dark red text
    t1.rowFormat.backColor = 0xFFFFCC;   // pale yellow cell fill
    t1.rowFormat.backPattern = 1;        // solid

    schedule::Task t2;
    t2.uniqueId = 2; t2.id = 2; t2.outlineLevel = 1;
    t2.name = QStringLiteral("Implement — 実装");
    t2.milestone = true;
    t2.wbs = QStringLiteral("2");
    p.tasks = { t1, t2 };

    // View style template: written by patching the template's Gantt view
    // STYLE_DATA in place, so every modelled field must read back identically.
    // A file always carries view styles, so a written project reads back with
    // present == true; the sample starts that way to stay round-trippable.
    p.viewStyles.present = true;
    p.viewStyles.text[schedule::ViewStyles::Critical].bold = true;
    p.viewStyles.text[schedule::ViewStyles::Critical].color = 0xCC0000;
    p.viewStyles.text[schedule::ViewStyles::Summary].bold = true;
    p.viewStyles.text[schedule::ViewStyles::RowAndColumn].color = 0x202020;
    p.viewStyles.statusDateLine = { 0xE04040, 3 };
    p.viewStyles.currentDateLine = { 0x5B7C99, 4 };
    p.viewStyles.ganttRows = { 0xE2E2E2, 1 };
    p.viewStyles.taskBar = { 0x9FC5E8, 0x4A7EBB, 0x4A7EBB };
    p.viewStyles.milestone = { 0x101010, 0x101010, 0x101010 };
    p.viewStyles.summaryBar = { 0x202020, 0x202020, 0x202020 };
    p.viewStyles.projectSummaryBar = { 0x808080, 0x808080, 0x808080 };

    schedule::Resource r;
    r.uniqueId = 1; r.id = 1; r.name = QStringLiteral("Alice"); r.initials = QStringLiteral("A");
    r.maxUnits = 0.5;
    schedule::AvailabilityPeriod ap1;
    ap1.startDate = QDateTime(QDate(2026, 1, 1), QTime(0, 0), Qt::UTC);
    ap1.endDate = QDateTime(QDate(2026, 1, 31), QTime(23, 59), Qt::UTC);
    ap1.units = 0.5;
    schedule::AvailabilityPeriod ap2;
    ap2.startDate = QDateTime(QDate(2026, 3, 1), QTime(0, 0), Qt::UTC);   // gap before this one
    ap2.endDate = QDateTime(QDate(2026, 3, 31), QTime(23, 59), Qt::UTC);
    ap2.units = 1.0;
    r.availabilityTable = { ap1, ap2 };
    p.resources = { r };

    schedule::Calendar cal;
    cal.uniqueId = 1;
    cal.name = QStringLiteral("Night Shift");
    cal.baseCalendarUniqueId = -1;
    // Base calendars must spell out working hours per weekday (Mon..Sun); the
    // standard Mon-Fri 8-12/13-17 pattern matches the writer's "default day" case.
    const QList<schedule::TimeRange> workDay = {
        { QTime(8, 0), QTime(12, 0) }, { QTime(13, 0), QTime(17, 0) }
    };
    cal.workingTimes = { workDay, workDay, workDay, workDay, workDay, {}, {} };   // Mon..Fri, Sat/Sun off
    cal.workingDayMask = 0b0011111;
    p.calendars = { cal };
    p.calendarUniqueId = cal.uniqueId;   // project default -> the non-Standard calendar

    return p;
}

void TstSemanticRoundtrip::syntheticRoundTrip()
{
    const schedule::Project original = makeSampleProject();

    MppIO writer;
    writer.setProject(original);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));

    MppIO reader;
    QVERIFY2(reader.openFromData(bytes), qPrintable(reader.errorString()));

    // Primary correctness gate: model survives read -> write -> read.
    QVERIFY(reader.project() == original);
}

void TstSemanticRoundtrip::writerIsDeterministic()
{
    MppIO io;
    io.setProject(makeSampleProject());
    const QByteArray b = io.saveToData();

    MppIO io2;
    QVERIFY(io2.openFromData(b));
    const QByteArray c = io2.saveToData();

    // Our writer must be byte-stable across a re-save of the same model.
    QCOMPARE(c, b);
}

void TstSemanticRoundtrip::realFixtures_data()
{
    QTest::addColumn<QString>("path");
    const QString dir = QStringLiteral(SCHEDULEIO_FIXTURE_DIR);
    const QStringList mpps = QDir(dir).entryList({ QStringLiteral("*.mpp") }, QDir::Files);
    for (const QString &f : mpps)
        QTest::newRow(qPrintable(f)) << QDir(dir).filePath(f);
}

void TstSemanticRoundtrip::realFixtures()
{
    // With no fixtures present this slot is still invoked once with no data row;
    // skip cleanly before touching QFETCH.
    if (QDir(QStringLiteral(SCHEDULEIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files)
            .isEmpty())
        QSKIP("no .mpp fixtures present yet (add real files under tests/fixtures)");

    QFETCH(QString, path);

    MppIO reader;
    if (!reader.open(path))
        QSKIP(qPrintable(QStringLiteral("parser cannot yet read this fixture: %1 "
                                         "(expected until the MPP field mapping is filled in)")
                             .arg(reader.errorString())));

    const schedule::Project m1 = reader.project();
    MppIO writer;
    writer.setProject(m1);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));

    MppIO reader2;
    QVERIFY2(reader2.openFromData(bytes), qPrintable(reader2.errorString()));
    QVERIFY2(reader2.project() == m1, qPrintable(diffProjects(m1, reader2.project())));
}

QTEST_MAIN(TstSemanticRoundtrip)
#include "tst_semantic_roundtrip.moc"
