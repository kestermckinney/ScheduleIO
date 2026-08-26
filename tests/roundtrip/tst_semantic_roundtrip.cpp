// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"
#include "codec/bkndvardata.h"
#include "codec/fielddecoders.h"
#include "ole/compoundfile.h"
#include "serializer/mpp14manifest.h"

#include <QDir>
#include <QTest>
#include <QtEndian>

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
    if (a.scheduleFromStart != b.scheduleFromStart) d << QStringLiteral("scheduleFromStart");
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
        if (x.physicalPercentComplete != y.physicalPercentComplete)
            f << QStringLiteral("physicalPct");
        if (x.earnedValueMethod != y.earnedValueMethod)
            f << QStringLiteral("earnedValueMethod");
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
        if (x.ignoreResourceCalendar != y.ignoreResourceCalendar)
            f << QStringLiteral("ignoreResourceCalendar");
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
        if (x.rowFormat != y.rowFormat)
            f << QStringLiteral("rowFormat b%1%2 i%3%4 u%5%6 s%7%8 color %9/%10 back %11/%12 pat %13/%14")
                     .arg(x.rowFormat.bold).arg(y.rowFormat.bold)
                     .arg(x.rowFormat.italic).arg(y.rowFormat.italic)
                     .arg(x.rowFormat.underline).arg(y.rowFormat.underline)
                     .arg(x.rowFormat.strikethrough).arg(y.rowFormat.strikethrough)
                     .arg(x.rowFormat.color).arg(y.rowFormat.color)
                     .arg(x.rowFormat.backColor).arg(y.rowFormat.backColor)
                     .arg(x.rowFormat.backPattern).arg(y.rowFormat.backPattern);
        if (x.rowFormat.fontName != y.rowFormat.fontName
            || x.rowFormat.fontSize != y.rowFormat.fontSize)
            f << QStringLiteral("rowFont '%1'/%2 vs '%3'/%4")
                     .arg(x.rowFormat.fontName).arg(x.rowFormat.fontSize)
                     .arg(y.rowFormat.fontName).arg(y.rowFormat.fontSize);
        if (x.cellFormats != y.cellFormats)
            f << QStringLiteral("cellFormats keys %1 vs %2")
                     .arg(x.cellFormats.keys().join(QLatin1Char(',')),
                          y.cellFormats.keys().join(QLatin1Char(',')));
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
        if (x.costRateTable != y.costRateTable)
            f << QStringLiteral("costRateTable %1/%2").arg(x.costRateTable).arg(y.costRateTable);
        if (x.variableRateUnits != y.variableRateUnits)
            f << QStringLiteral("variableRateUnits %1/%2").arg(x.variableRateUnits).arg(y.variableRateUnits);
        if (x.workContour != y.workContour)
            f << QStringLiteral("workContour %1/%2").arg(x.workContour).arg(y.workContour);
        if (x.workMillis != y.workMillis) f << QStringLiteral("work %1/%2").arg(x.workMillis).arg(y.workMillis);
        if (x.actualWorkMillis != y.actualWorkMillis)
            f << QStringLiteral("actualWork %1/%2").arg(x.actualWorkMillis).arg(y.actualWorkMillis);
        if (x.remainingWorkMillis != y.remainingWorkMillis)
            f << QStringLiteral("remainingWork %1/%2").arg(x.remainingWorkMillis).arg(y.remainingWorkMillis);
        if (x.start != y.start)
            f << QStringLiteral("start %1/%2").arg(x.start.toString(Qt::ISODate), y.start.toString(Qt::ISODate));
        if (x.finish != y.finish)
            f << QStringLiteral("finish %1/%2").arg(x.finish.toString(Qt::ISODate), y.finish.toString(Qt::ISODate));
        if (x.stop != y.stop)
            f << QStringLiteral("stop %1/%2").arg(x.stop.toString(Qt::ISODate), y.stop.toString(Qt::ISODate));
        if (x.resume != y.resume)
            f << QStringLiteral("resume %1/%2").arg(x.resume.toString(Qt::ISODate), y.resume.toString(Qt::ISODate));
        if (x.timephasedValues != y.timephasedValues) {
            f << QStringLiteral("timephasedValues (%1/%2)")
                     .arg(x.timephasedValues.size()).arg(y.timephasedValues.size());
            if (x.timephasedValues.size() != y.timephasedValues.size()) {
                const auto &xv = x.timephasedValues.constLast();
                const QString yLast = y.timephasedValues.isEmpty()
                    ? QStringLiteral("<none>")
                    : QStringLiteral("%1..%2 %3")
                          .arg(y.timephasedValues.constLast().start.toString(Qt::ISODate),
                               y.timephasedValues.constLast().finish.toString(Qt::ISODate),
                               y.timephasedValues.constLast().value);
                f << QStringLiteral("  last type%1 %2..%3 %4 / %5")
                         .arg(xv.type)
                         .arg(xv.start.toString(Qt::ISODate),
                              xv.finish.toString(Qt::ISODate), xv.value, yLast);
            }
            for (int n = 0; n < qMin(x.timephasedValues.size(), y.timephasedValues.size()); ++n) {
                const auto &xv = x.timephasedValues.at(n);
                const auto &yv = y.timephasedValues.at(n);
                if (!(xv == yv)) {
                    f << QStringLiteral("  tp#%1 type %2/%3 start %4/%5 finish %6/%7 value %8/%9")
                             .arg(n).arg(xv.type).arg(yv.type)
                             .arg(xv.start.toString(Qt::ISODate), yv.start.toString(Qt::ISODate))
                             .arg(xv.finish.toString(Qt::ISODate), yv.finish.toString(Qt::ISODate))
                             .arg(xv.value, yv.value);
                    break;
                }
            }
        }
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

#include "fixtureutils.h"

class TstSemanticRoundtrip : public QObject
{
    Q_OBJECT
private slots:
    void syntheticRoundTrip();
    void writerIsDeterministic();
    void taskDisplayOrderOrdinalIsWritten();
    void inconsistentProgressIsCanonicalizedForProject();
    void completedAssignmentStateIsWritten();
    void timephasedAssignmentRoundTrip();
    void nativeParityMetadataRoundTrip();
    void nativeCostRateHeaderIsWritten();
    void nativePhysicalProgressFieldsAreWritten();
    void fontBaseTableAndIndicesArePreserved();
    void normalRowWeightIsWrittenExplicitly();
    void parentRowsetManifestsAreConsistent();
    void timelineViewSurvivesEditedFixtureSave();
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
    p.startDate = QDateTime(QDate(2026, 1, 2), QTime(8, 0));
    p.finishDate = QDateTime(QDate(2026, 3, 31), QTime(17, 0));
    p.scheduleFromStart = false;

    schedule::Task t1;
    t1.uniqueId = 1; t1.id = 1; t1.outlineLevel = 1;
    t1.name = QStringLiteral("Design");
    t1.start = QDateTime(QDate(2026, 1, 2), QTime(9, 0));
    t1.finish = QDateTime(QDate(2026, 1, 9), QTime(17, 0));
    t1.durationMillis = qint64(8) * 3600 * 1000;   // divisible by the duration unit
    t1.percentComplete = 0.5;
    t1.physicalPercentComplete = 0.35;
    t1.earnedValueMethod = 1;
    t1.workMillis = qint64(16) * 3600 * 1000;                // task-level Work
    t1.levelingDelayMillis = qint64(4) * 3600 * 1000;        // resource-leveling delay
    t1.ignoreResourceCalendar = true;
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
    t1.rowFormat.fontName = QStringLiteral("Arial");
    t1.rowFormat.fontSize = 10;
    schedule::TextStyle durationCell;
    durationCell.color = 0x0044CC;
    durationCell.backColor = 0xFFF080;
    durationCell.backPattern = 1;
    t1.cellFormats.insert(QStringLiteral("5"), durationCell); // ScheduleVault Duration key

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
    ap1.startDate = QDateTime(QDate(2026, 1, 1), QTime(0, 0));
    ap1.endDate = QDateTime(QDate(2026, 1, 31), QTime(23, 59));
    ap1.units = 0.5;
    schedule::AvailabilityPeriod ap2;
    ap2.startDate = QDateTime(QDate(2026, 3, 1), QTime(0, 0));   // gap before this one
    ap2.endDate = QDateTime(QDate(2026, 3, 31), QTime(23, 59));
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
    QVERIFY2(reader.project() == original, qPrintable(diffProjects(original, reader.project())));
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

void TstSemanticRoundtrip::taskDisplayOrderOrdinalIsWritten()
{
    schedule::Project p = makeSampleProject();
    // Deliberately make display order differ from unique-ID order. The MPP task
    // records themselves are UID-ordered, while Fixed2Data+16 must carry ID+1
    // so Microsoft Project can reconstruct the display outline.
    p.tasks[0].uniqueId = 20;
    p.tasks[0].id = 1;
    p.tasks[1].uniqueId = 5;
    p.tasks[1].id = 2;

    MppIO io;
    io.setProject(p);
    const QByteArray bytes = io.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(io.errorString()));

    CompoundFile cf;
    QVERIFY2(cf.openFromData(bytes), qPrintable(cf.errorString()));
    const QStringList base = { QStringLiteral("   114"), QStringLiteral("TBkndTask") };
    const QByteArray meta = cf.readStream(base + QStringList{ QStringLiteral("Fixed2Meta") });
    const QByteArray data = cf.readStream(base + QStringList{ QStringLiteral("Fixed2Data") });
    QVERIFY(meta.size() >= 16 + 5 * 96);

    const auto u32 = [](const QByteArray &b, int off) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + off));
    };
    const auto f64 = [](const QByteArray &b, int off) {
        const quint64 bits = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar *>(b.constData() + off));
        double value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    };

    // Items 0..2 are the format's placeholder rows. Item 3 is UID 5 (ID 2),
    // item 4 is UID 20 (ID 1).
    const int first = int(u32(meta, 16 + 3 * 96 + 4));
    const int second = int(u32(meta, 16 + 4 * 96 + 4));
    QCOMPARE(f64(data, first + 16), 3.0);
    QCOMPARE(f64(data, second + 16), 2.0);

    const QStringList resourceBase = {
        QStringLiteral("   114"), QStringLiteral("TBkndRsc") };
    const QByteArray resourceMeta = cf.readStream(
        resourceBase + QStringList{QStringLiteral("Fixed2Meta")});
    const QByteArray resourceData = cf.readStream(
        resourceBase + QStringList{QStringLiteral("Fixed2Data")});
    const QByteArray resourceFixedMeta = cf.readStream(
        resourceBase + QStringList{QStringLiteral("FixedMeta")});
    const QByteArray resourceFixedData = cf.readStream(
        resourceBase + QStringList{QStringLiteral("FixedData")});
    QVERIFY(resourceMeta.size() >= 16 + 5 * 51);
    const int resourceOffset = int(u32(resourceMeta, 16 + 4 * 51 + 4));
    QVERIFY(resourceOffset + 40 <= resourceData.size());
    const QByteArray resourceRecord = resourceData.mid(resourceOffset, 40);
    QVERIFY(resourceRecord.left(16) != QByteArray(16, '\0'));
    QCOMPARE(quint8(resourceRecord.at(6)) & 0xF0u, quint8(0x10)); // UUID v1
    QCOMPARE(resourceRecord.mid(4, 12),
             QByteArray::fromHex("81a411f1913532894ab24c0f"));
    QCOMPARE(f64(resourceRecord, 16), double(p.resources.first().id + 1));
    QCOMPARE(resourceRecord.mid(24, 16), resourceRecord.left(16));
    const int resourceFixedOffset = int(u32(resourceFixedMeta, 16 + 4 * 37 + 4));
    QCOMPARE(u32(resourceFixedData, resourceFixedOffset + 154),
             quint32(p.resources.first().id + 2));
}

void TstSemanticRoundtrip::completedAssignmentStateIsWritten()
{
    schedule::Project p = makeSampleProject();
    schedule::Assignment a;
    a.uniqueId = 10;
    a.taskUniqueId = p.tasks.first().uniqueId;
    a.resourceUniqueId = p.resources.first().uniqueId;
    a.units = 1.0;
    a.workMillis = qint64(8) * 3600 * 1000;
    a.actualWorkMillis = a.workMillis;
    a.remainingWorkMillis = 0;
    a.start = p.tasks.first().start;
    a.finish = p.tasks.first().finish;
    p.assignments = { a };

    MppIO io;
    io.setProject(p);
    QCOMPARE(io.project().assignments.first().overtimeWorkMillis, qint64(0));
    const QByteArray bytes = io.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(io.errorString()));

    CompoundFile cf;
    QVERIFY(cf.openFromData(bytes));
    const QStringList base = { QStringLiteral("   114"), QStringLiteral("TBkndAssn") };
    const QByteArray meta = cf.readStream(base + QStringList{ QStringLiteral("FixedMeta") });
    const QByteArray data = cf.readStream(base + QStringList{ QStringLiteral("FixedData") });
    QVERIFY(meta.size() >= 16 + 4 * 34);
    const int off = int(qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(meta.constData() + 16 + 3 * 34 + 4)));
    const auto u16 = [&](int fieldOff) {
        return qFromLittleEndian<quint16>(
            reinterpret_cast<const uchar *>(data.constData() + off + fieldOff));
    };
    const auto u32 = [&](int fieldOff) {
        return qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(data.constData() + off + fieldOff));
    };
    const auto f64 = [&](int fieldOff) {
        quint64 bits = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar *>(data.constData() + off + fieldOff));
        double value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    };

    QCOMPARE(f64(20), f64(28));    // WORK == ACTUAL_WORK
    QCOMPARE(f64(20), f64(36));    // WORK == REGULAR_WORK
    QCOMPARE(f64(44), 0.0);        // REMAINING_WORK
    QCOMPARE(u32(56), u32(60));    // FINISH == RESUME
    QCOMPARE(u32(56), u32(104));   // FINISH == STOP
    QCOMPARE(u16(92), quint16(7)); // LEVELING_DELAY_UNITS
}

void TstSemanticRoundtrip::nativeCostRateHeaderIsWritten()
{
    schedule::Project project = makeSampleProject();
    schedule::CostRate rate;
    rate.table = 0;
    rate.standardRate = 100.0;
    rate.overtimeRate = 150.0;
    rate.costPerUse = 25.0;
    project.resources[0].costRates = { rate };

    MppIO io;
    io.setProject(project);
    const QByteArray bytes = io.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(io.errorString()));

    CompoundFile cf;
    QVERIFY2(cf.openFromData(bytes), qPrintable(cf.errorString()));
    const QStringList base = { QStringLiteral("   114"), QStringLiteral("TBkndRsc") };
    BkndVarData varData;
    QVERIFY(varData.parse(
        cf.readStream(base + QStringList{ QStringLiteral("VarMeta") }),
        cf.readStream(base + QStringList{ QStringLiteral("Var2Data") })));
    const QByteArray blob = varData.blobFor(1, 61);
    QCOMPARE(blob.size(), 60);
    QCOMPARE(qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(blob.constData())),
             quint16(1));
    QCOMPARE(qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(blob.constData() + 2)),
             quint16(4));
    QCOMPARE(qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(blob.constData() + 4)),
             quint16(16));
}

void TstSemanticRoundtrip::nativePhysicalProgressFieldsAreWritten()
{
    const schedule::Project project = makeSampleProject();
    const schedule::Task &task = project.tasks.first();

    MppIO io;
    io.setProject(project);
    const QByteArray bytes = io.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(io.errorString()));

    CompoundFile cf;
    QVERIFY2(cf.openFromData(bytes), qPrintable(cf.errorString()));
    const QStringList base = { QStringLiteral("   114"), QStringLiteral("TBkndTask") };
    BkndVarData varData;
    QVERIFY(varData.parse(
        cf.readStream(base + QStringList{ QStringLiteral("VarMeta") }),
        cf.readStream(base + QStringList{ QStringLiteral("Var2Data") })));
    const QByteArray physical = varData.blobFor(task.uniqueId, 1119);
    const QByteArray method = varData.blobFor(task.uniqueId, 1122);
    QCOMPARE(physical.size(), 2);
    QCOMPARE(method.size(), 2);
    QCOMPARE(qFromLittleEndian<quint16>(
                 reinterpret_cast<const uchar *>(physical.constData())), quint16(35));
    QCOMPARE(qFromLittleEndian<quint16>(
                 reinterpret_cast<const uchar *>(method.constData())), quint16(1));
}

void TstSemanticRoundtrip::timephasedAssignmentRoundTrip()
{
    schedule::Project p = makeSampleProject();
    schedule::Assignment assignment;
    assignment.uniqueId = 10;
    assignment.taskUniqueId = p.tasks.first().uniqueId;
    assignment.resourceUniqueId = p.resources.first().uniqueId;
    assignment.units = 1.0;
    assignment.workMillis = 20LL * 3600 * 1000;
    assignment.actualWorkMillis = 4LL * 3600 * 1000;
    assignment.remainingWorkMillis = 16LL * 3600 * 1000;
    assignment.start = QDateTime(QDate(2026, 1, 5), QTime(8, 0));
    assignment.stop = QDateTime(QDate(2026, 1, 5), QTime(12, 0));
    assignment.resume = QDateTime(QDate(2026, 1, 6), QTime(8, 0));
    assignment.finish = QDateTime(QDate(2026, 1, 7), QTime(17, 0));

    auto addBucket = [&](int type, const QDate &day, int hours,
                         const QTime &finish = QTime(17, 0)) {
        schedule::TimephasedValue value;
        value.type = type;
        value.uniqueId = assignment.uniqueId;
        value.start = QDateTime(day, QTime(8, 0));
        value.finish = QDateTime(day, finish);
        value.unit = 1;
        value.value = QStringLiteral("PT%1H0M0S").arg(hours);
        assignment.timephasedValues.append(value);
    };
    addBucket(schedule::TimephasedValue::ActualWork, QDate(2026, 1, 5), 4, QTime(12, 0));
    addBucket(schedule::TimephasedValue::RemainingWork, QDate(2026, 1, 6), 6);
    addBucket(schedule::TimephasedValue::RemainingWork, QDate(2026, 1, 7), 10);
    // Match a daily Resource Usage edit: replace the Jan 6 cell while preserving
    // Jan 5 actuals and Jan 7 remaining work.
    QVERIFY(assignment.setTimephasedWorkInPeriod(
        schedule::TimephasedValue::RemainingWork,
        QDateTime(QDate(2026, 1, 6), QTime(0, 0)),
        QDateTime(QDate(2026, 1, 7), QTime(0, 0)),
        8LL * 3600 * 1000));
    QCOMPARE(assignment.remainingWorkMillis, 18LL * 3600 * 1000);
    QCOMPARE(assignment.workMillis, 22LL * 3600 * 1000);
    p.assignments = { assignment };

    MppIO writer;
    writer.setProject(p);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));

    MppIO reader;
    QVERIFY2(reader.openFromData(bytes), qPrintable(reader.errorString()));
    QCOMPARE(reader.project().assignments.size(), 1);
    const schedule::Assignment &decoded = reader.project().assignments.first();
    QCOMPARE(decoded.stop, assignment.stop);
    QCOMPARE(decoded.resume, assignment.resume);

    QHash<QString, qint64> daily;
    for (const schedule::TimephasedValue &value : decoded.timephasedValues) {
        const QString key = QStringLiteral("%1|%2")
            .arg(value.type).arg(value.start.date().toString(Qt::ISODate));
        daily[key] += value.durationMillis();
    }
    QCOMPARE(daily.value(QStringLiteral("2|2026-01-05")), 4LL * 3600 * 1000);
    QCOMPARE(daily.value(QStringLiteral("1|2026-01-06")), 8LL * 3600 * 1000);
    QCOMPARE(daily.value(QStringLiteral("1|2026-01-07")), 10LL * 3600 * 1000);
}

void TstSemanticRoundtrip::nativeParityMetadataRoundTrip()
{
    schedule::Project p = makeSampleProject();
    p.multipleCriticalPaths = true;
    schedule::CalendarException exception;
    exception.name = QStringLiteral("Biweekly shutdown");
    exception.fromDate = QDate(2026, 1, 7);
    exception.toDate = QDate(2026, 3, 31);
    exception.recurrence = schedule::CalendarException::Recurrence::Weekly;
    exception.interval = 2;
    exception.weekDayMask = 1u << 2; // Wednesday
    exception.occurrences = 5;
    p.calendars.first().exceptions.append(exception);

    schedule::Resource &resource = p.resources.first();
    resource.budget = true;
    resource.budgetWorkMillis = 40LL * 3600 * 1000;
    resource.budgetCost = 12000.0;
    schedule::Assignment assignment;
    assignment.uniqueId = 20;
    assignment.taskUniqueId = 0;
    assignment.resourceUniqueId = resource.uniqueId;
    assignment.budget = true;
    assignment.budgetWorkMillis = resource.budgetWorkMillis;
    assignment.budgetCost = resource.budgetCost;
    assignment.start = p.startDate;
    assignment.finish = p.finishDate;
    schedule::TimephasedValue work;
    work.type = schedule::TimephasedValue::BaselineWork;
    work.uniqueId = assignment.uniqueId;
    work.baselineNumber = 2;
    work.start = QDateTime(QDate(2026, 1, 5), QTime(8, 0));
    work.finish = QDateTime(QDate(2026, 1, 5), QTime(12, 0));
    work.unit = 1;
    work.value = QStringLiteral("PT4H0M0S");
    schedule::TimephasedValue cost = work;
    cost.type = schedule::TimephasedValue::BaselineCost;
    cost.unit = 2;
    cost.value = QStringLiteral("375.5");
    assignment.timephasedValues = {work, cost};
    p.assignments = {assignment};

    MppIO writer;
    writer.setProject(p);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));
    MppIO reader;
    QVERIFY2(reader.openFromData(bytes), qPrintable(reader.errorString()));
    const schedule::Project &decoded = reader.project();
    QVERIFY(decoded.multipleCriticalPaths);
    QVERIFY(decoded.resources.first().budget);
    QCOMPARE(decoded.resources.first().budgetWorkMillis, resource.budgetWorkMillis);
    QCOMPARE(decoded.resources.first().budgetCost, resource.budgetCost);
    QCOMPARE(decoded.assignments.first().budgetWorkMillis, assignment.budgetWorkMillis);
    QCOMPARE(decoded.assignments.first().budgetCost, assignment.budgetCost);
    QCOMPARE(decoded.budgetCost, assignment.budgetCost);
    const auto &decodedException = decoded.calendars.first().exceptions.first();
    QCOMPARE(decodedException.recurrence,
             schedule::CalendarException::Recurrence::Weekly);
    QCOMPARE(decodedException.interval, 2);
    QCOMPARE(decodedException.weekDayMask, quint8(1u << 2));
    QCOMPARE(decodedException.occurrences, 5);
    bool foundWork = false, foundCost = false;
    for (const auto &value : decoded.assignments.first().timephasedValues) {
        if (value.type == schedule::TimephasedValue::BaselineWork
            && value.baselineNumber == 2) {
            QCOMPARE(value.durationMillis(), 4LL * 3600 * 1000);
            foundWork = true;
        }
        if (value.type == schedule::TimephasedValue::BaselineCost
            && value.baselineNumber == 2) {
            QCOMPARE(value.value.toDouble(), 375.5);
            foundCost = true;
        }
    }
    QVERIFY(foundWork);
    QVERIFY(foundCost);
}

void TstSemanticRoundtrip::fontBaseTableAndIndicesArePreserved()
{
    MppIO seedWriter;
    seedWriter.setProject(makeSampleProject());
    const QByteArray seedBytes = seedWriter.saveToData();
    QVERIFY(!seedBytes.isEmpty());

    MppIO seedReader;
    QVERIFY(seedReader.openFromData(seedBytes));
    schedule::Project p = seedReader.project();
    QVERIFY(p.mppFontBases.size() > 2 + 10 * 68 + 6);
    p.mppFontBases[2 + 10 * 68 + 4] = 'X'; // opaque unused-entry marker
    p.viewStyles.text[schedule::ViewStyles::Critical].fontBaseIndex = 5;

    MppIO writer;
    writer.setProject(p);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));

    MppIO reader;
    QVERIFY(reader.openFromData(bytes));
    QCOMPARE(reader.project().mppFontBases, p.mppFontBases);
    const schedule::TextStyle &critical =
        reader.project().viewStyles.text[schedule::ViewStyles::Critical];
    QCOMPARE(critical.fontBaseIndex, 5);
    // The family/size must be resolved from the font-base table on read, not left
    // empty -- consumers like the Gantt view's task-detail text read fontName/
    // fontSize directly and never look at fontBaseIndex.
    QVERIFY(!critical.fontName.isEmpty());
    QVERIFY(critical.fontSize > 0);
}

void TstSemanticRoundtrip::inconsistentProgressIsCanonicalizedForProject()
{
    schedule::Project p = makeSampleProject();
    schedule::Task &partial = p.tasks[0];
    partial.percentComplete = 0.4;
    partial.actualStart = partial.start;
    partial.actualFinish = partial.finish;                 // stale completed state
    partial.actualDurationMillis = partial.durationMillis; // contradicts 40%
    partial.actualWorkMillis = partial.workMillis;         // contradicts 40%

    schedule::Task &unstarted = p.tasks[1];
    unstarted.start = QDateTime(QDate(2026, 1, 12), QTime(9, 0));
    unstarted.finish = QDateTime(QDate(2026, 1, 12), QTime(17, 0));
    unstarted.durationMillis = 8LL * 3600 * 1000;
    unstarted.workMillis = 8LL * 3600 * 1000;
    unstarted.percentComplete = 0.0;

    schedule::Assignment staleAssignment;
    staleAssignment.uniqueId = 10;
    staleAssignment.taskUniqueId = unstarted.uniqueId;
    staleAssignment.resourceUniqueId = -65535;
    staleAssignment.workMillis = unstarted.workMillis;
    staleAssignment.remainingWorkMillis = staleAssignment.workMillis;
    staleAssignment.start = unstarted.start.addMonths(-2);
    staleAssignment.finish = unstarted.finish.addMonths(-2);
    p.assignments.append(staleAssignment);

    MppIO writer;
    writer.setProject(p);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));

    // Project consults the remaining-duration copy when categorising and
    // scheduling a row, so verify the binary field itself rather than relying
    // only on ScheduleIO's reader (which intentionally does not model it).
    CompoundFile cf;
    QVERIFY2(cf.openFromData(bytes), qPrintable(cf.errorString()));
    const QStringList base = { QStringLiteral("   114"), QStringLiteral("TBkndTask") };
    const QByteArray meta = cf.readStream(base + QStringList{ QStringLiteral("FixedMeta") });
    const QByteArray data = cf.readStream(base + QStringList{ QStringLiteral("FixedData") });
    const auto u16 = [](const QByteArray &b, int off) {
        return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(b.constData() + off));
    };
    const auto u32 = [](const QByteArray &b, int off) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + off));
    };
    auto recordForUid = [&](quint32 wanted, int *metaItem) {
        const int count = int(u32(meta, 8));
        for (int i = 0; i < count; ++i) {
            const int item = 16 + i * 47;
            const int off = int(u32(meta, item + 4));
            if (off + 202 <= data.size() && u32(data, off + 4) == wanted) {
                if (metaItem)
                    *metaItem = item;
                return data.mid(off, 202);
            }
        }
        return QByteArray();
    };

    int partialMeta = -1;
    const QByteArray partialRecord = recordForUid(quint32(partial.uniqueId), &partialMeta);
    QCOMPARE(partialRecord.size(), 202);
    QCOMPARE(u32(partialRecord, 88), quint32(2880)); // 60% of 8h, tenth-minutes
    QCOMPARE(u16(partialRecord, 92), quint16(40));
    QCOMPARE(u16(partialRecord, 140), quint16(0));   // leaf, not summary
    QCOMPARE(u16(partialRecord, 164), quint16(0x07)); // automatic leaf state
    QCOMPARE(u32(partialRecord, 168), quint32(0x00002080));
    QVERIFY(partialMeta >= 0);
    QCOMPARE(uchar(meta.at(partialMeta + 12)) & 0x0c, 0); // neither Rollup nor Summary
    QCOMPARE(uchar(meta.at(partialMeta + 9)) & 0x40, 0);  // no project-summary status bit
    QCOMPARE(uchar(meta.at(partialMeta + 13)) & 0x10, 0); // no project-summary format bit
    QVERIFY(uchar(meta.at(partialMeta + 17)) & 0x04);      // normal automatic-task state

    int unstartedMeta = -1;
    const QByteArray unstartedRecord = recordForUid(quint32(unstarted.uniqueId), &unstartedMeta);
    QCOMPARE(unstartedRecord.size(), 202);
    QCOMPARE(u32(unstartedRecord, 88), quint32(4800)); // full 8h remains
    QCOMPARE(u16(unstartedRecord, 92), quint16(0));
    QVERIFY(unstartedMeta >= 16);

    const QStringList assnBase = { QStringLiteral("   114"), QStringLiteral("TBkndAssn") };
    const QByteArray assnMeta = cf.readStream(assnBase + QStringList{ QStringLiteral("FixedMeta") });
    const QByteArray assnData = cf.readStream(assnBase + QStringList{ QStringLiteral("FixedData") });
    const QByteArray assnF2Meta = cf.readStream(assnBase + QStringList{ QStringLiteral("Fixed2Meta") });
    const QByteArray assnF2Data = cf.readStream(assnBase + QStringList{ QStringLiteral("Fixed2Data") });
    QByteArray staleAssnRecord;
    int staleAssnIndex = -1;
    const int assnCount = (assnMeta.size() - 16) / 34;
    QCOMPARE(assnCount, 4); // three native placeholders plus one live row
    QCOMPARE(int(u32(assnF2Meta, 8)), 4);
    QCOMPARE(u32(assnF2Meta, 16 + 0 * 53 + 4), quint32(0));
    QCOMPARE(u32(assnF2Meta, 16 + 1 * 53 + 4), quint32(48));
    QCOMPARE(u32(assnF2Meta, 16 + 2 * 53 + 4), quint32(96));
    for (int i = 0; i < assnCount; ++i) {
        const int item = 16 + i * 34;
        const int off = int(u32(assnMeta, item + 4));
        if (off + 110 <= assnData.size() && u32(assnData, off) == 10) {
            staleAssnRecord = assnData.mid(off, 110);
            QCOMPARE(u32(assnMeta, item), quint32(0x00090000));
            QCOMPARE(uchar(assnMeta.at(item + 10)), uchar(0x23));
            QCOMPARE(uchar(assnMeta.at(item + 32)), uchar(0x40));
            staleAssnIndex = i;
            break;
        }
    }
    QCOMPARE(staleAssnRecord.size(), 110);
    const quint32 assignmentStart = FieldDecoders::encodeMppTimestamp(unstarted.start);
    QCOMPARE(u32(staleAssnRecord, 60), assignmentStart);  // Resume
    QCOMPARE(u32(staleAssnRecord, 104), assignmentStart); // Stop
    QVERIFY(staleAssnIndex >= 0);
    const int assnF2Item = 16 + staleAssnIndex * 53;
    QCOMPARE(u32(assnF2Meta, assnF2Item), quint32(0));
    QCOMPARE(u32(assnF2Meta, assnF2Item + 8), quint32(0x0000084F));
    const int assnF2Off = int(u32(assnF2Meta, assnF2Item + 4));
    const QByteArray assnF2 = assnF2Data.mid(assnF2Off, 48);
    QCOMPARE(assnF2.size(), 48);
    QVERIFY(assnF2.left(16) != QByteArray(16, '\0')); // assignment GUID
    QVERIFY(assnF2.mid(16, 16) != QByteArray(16, '\0')); // task GUID
    QCOMPARE(assnF2.mid(32, 16), QByteArray::fromHex("788bcba08c2a6d4300000000000000ff"));
    QCOMPARE(uchar(assnF2Meta.at(assnF2Item + 35)), uchar(0x10));

    MppIO reader;
    QVERIFY2(reader.openFromData(bytes), qPrintable(reader.errorString()));
    const schedule::Task &readPartial = reader.project().tasks.at(0);
    QCOMPARE(readPartial.percentComplete, 0.4);
    QCOMPARE(readPartial.actualDurationMillis, qint64(192) * 60000); // 40% of 8h
    QCOMPARE(readPartial.actualWorkMillis, qint64(384) * 60000); // 40% of 16h work
    QVERIFY(!readPartial.actualFinish.isValid());

    const schedule::Task &readUnstarted = reader.project().tasks.at(1);
    QCOMPARE(readUnstarted.percentComplete, 0.0);
    QVERIFY(!readUnstarted.actualStart.isValid());
    QCOMPARE(reader.project().assignments.constLast().start, unstarted.start);
    QCOMPARE(reader.project().assignments.constLast().finish, unstarted.finish);
    QVERIFY(!readUnstarted.actualFinish.isValid());
    QCOMPARE(readUnstarted.actualDurationMillis, qint64(0));
    QCOMPARE(readUnstarted.actualWorkMillis, qint64(0));
    QVERIFY(!readUnstarted.summary);
    QVERIFY(!readUnstarted.manual);
}

void TstSemanticRoundtrip::normalRowWeightIsWrittenExplicitly()
{
    schedule::Project p = makeSampleProject();
    p.tasks[0].rowFormat.bold = false;

    MppIO writer;
    writer.setProject(p);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));

    CompoundFile cf;
    QVERIFY(cf.openFromData(bytes));
    const QStringList base = { QStringLiteral("   214"), QStringLiteral("CV_iew") };
    const QByteArray fixedMeta = cf.readStream(base + QStringList{ QStringLiteral("FixedMeta") });
    const QByteArray fixedData = cf.readStream(base + QStringList{ QStringLiteral("FixedData") });
    const auto u16 = [](const QByteArray &b, int off) {
        return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(b.constData() + off));
    };
    const auto u32 = [](const QByteArray &b, int off) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + off));
    };

    int ganttUid = -1;
    int lastOffset = -1;
    const int viewCount = int(u32(fixedMeta, 8));
    for (int i = 0; i < viewCount; ++i) {
        const int metaOff = 16 + i * 10;
        const int offset = int(u16(fixedMeta, metaOff + 4));
        if (offset <= lastOffset)
            continue;
        lastOffset = offset;
        if (offset + 138 <= fixedData.size() && u16(fixedData, offset + 110) == 0
            && u16(fixedData, offset + 112) == 1) {
            ganttUid = int(u32(fixedData, offset));
            break;
        }
    }
    QVERIFY(ganttUid >= 0);

    BkndVarData viewData;
    QVERIFY(viewData.parse(cf.readStream(base + QStringList{ QStringLiteral("VarMeta") }),
                           cf.readStream(base + QStringList{ QStringLiteral("Var2Data") })));
    const QByteArray props = viewData.blobFor(quint32(ganttUid), 6);
    QVERIFY(props.size() >= 16);

    QByteArray columnProperties;
    int itemOffset = 16;
    const int itemCount = int(u16(props, 12));
    for (int i = 0; i < itemCount && itemOffset + 12 <= props.size(); ++i) {
        const int size = int(u32(props, itemOffset));
        const quint32 key = u32(props, itemOffset + 4);
        itemOffset += 12;
        QVERIFY(itemOffset + size <= props.size());
        if (key == 574619660u)
            columnProperties = props.mid(itemOffset, size);
        itemOffset += size + (size & 1);
    }
    QVERIFY(!columnProperties.isEmpty());

    bool foundWholeRow = false;
    for (int off = 0; off + 44 <= columnProperties.size(); off += 44) {
        if (int(u32(columnProperties, off)) != p.tasks[0].uniqueId
            || u32(columnProperties, off + 4) != 0xFFFFFFFFu)
            continue;
        foundWholeRow = true;
        QCOMPARE(uchar(columnProperties.at(off + 11)) & 0x01, 0); // bold value: off
        QVERIFY(u16(columnProperties, off + 40) & 0x01);           // bold override: explicit
    }
    QVERIFY(foundWholeRow);
}

void TstSemanticRoundtrip::parentRowsetManifestsAreConsistent()
{
    MppIO writer;
    writer.setProject(makeSampleProject()); // also grows CV_iew Var2Data
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));

    CompoundFile cf;
    QVERIFY2(cf.openFromData(bytes), qPrintable(cf.errorString()));
    const QStringList issues = Mpp14Manifest::audit(cf);
    QVERIFY2(issues.isEmpty(), qPrintable(issues.join(QStringLiteral("\n"))));

    // Prove the auditor catches the exact stale-template failure that made a
    // populated task collection render as an empty schedule in Project.
    QByteArray props = cf.readStream({ QStringLiteral("   114"), QStringLiteral("Props") });
    int offset = 16;
    bool patched = false;
    while (offset + 12 <= props.size()) {
        const quint32 length = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(props.constData() + offset));
        const quint32 key = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(props.constData() + offset + 4));
        offset += 12;
        QVERIFY(offset + int(length) <= props.size());
        if (key == 0x01000001u && length >= 4) {
            qToLittleEndian<quint32>(4u,
                reinterpret_cast<uchar *>(props.data() + offset));
            patched = true;
            break;
        }
        offset += int(length);
    }
    QVERIFY(patched);
    cf.addStream({ QStringLiteral("   114"), QStringLiteral("Props") }, props);
    const QStringList brokenIssues = Mpp14Manifest::audit(cf);
    QVERIFY(std::any_of(brokenIssues.cbegin(), brokenIssues.cend(), [](const QString &issue) {
        return issue.contains(QStringLiteral("TBkndTask"))
            && issue.contains(QStringLiteral("declares 4 rows"));
    }));
}

void TstSemanticRoundtrip::timelineViewSurvivesEditedFixtureSave()
{
    QString fixturePath;
    for (const QString &path : fixtures::mppFiles()) {
        if (QFileInfo(path).fileName().compare(QStringLiteral("Timeline Test.mpp"),
                                               Qt::CaseInsensitive) == 0) {
            fixturePath = path;
            break;
        }
    }
    if (fixturePath.isEmpty())
        QSKIP("Timeline Test.mpp fixture is not installed");

    QFile fixture(fixturePath);
    QVERIFY2(fixture.open(QIODevice::ReadOnly), qPrintable(fixture.errorString()));
    const QByteArray sourceBytes = fixture.readAll();

    auto timelineUid = [](const CompoundFile &cf) {
        const QStringList base = { QStringLiteral("   214"), QStringLiteral("CV_iew") };
        const QByteArray meta = cf.readStream(base + QStringList{ QStringLiteral("FixedMeta") });
        const QByteArray data = cf.readStream(base + QStringList{ QStringLiteral("FixedData") });
        auto u16 = [](const QByteArray &b, int o) {
            return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(b.constData()) + o);
        };
        auto u32 = [](const QByteArray &b, int o) {
            return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData()) + o);
        };
        const int count = meta.size() >= 16 ? int(u32(meta, 8)) : 0;
        int lastOffset = -1;
        for (int i = 0; i < count; ++i) {
            const int mo = 16 + i * 10;
            if (mo + 10 > meta.size())
                break;
            const int offset = int(u16(meta, mo + 4));
            if (offset <= lastOffset)
                continue;
            lastOffset = offset;
            if (offset + 138 <= data.size() && u16(data, offset + 112) == 16)
                return int(u32(data, offset));
        }
        return -1;
    };

    CompoundFile sourceCf;
    QVERIFY(sourceCf.openFromData(sourceBytes));
    const int sourceTimelineUid = timelineUid(sourceCf);
    QVERIFY(sourceTimelineUid >= 0);
    BkndVarData sourceViews;
    QVERIFY(sourceViews.parse(
        sourceCf.readStream({ QStringLiteral("   214"), QStringLiteral("CV_iew"), QStringLiteral("VarMeta") }),
        sourceCf.readStream({ QStringLiteral("   214"), QStringLiteral("CV_iew"), QStringLiteral("Var2Data") })));
    const QByteArray sourceMembership = sourceViews.blobFor(quint32(sourceTimelineUid), 47);
    QVERIFY(!sourceMembership.isEmpty());

    MppIO reader;
    QVERIFY2(reader.openFromData(sourceBytes), qPrintable(reader.errorString()));
    schedule::Project edited = reader.project();
    edited.title += QStringLiteral(" edited"); // force a semantic rewrite

    // Match ScheduleVault's save path: it creates a fresh writer and passes
    // the edited value model to setProject().
    MppIO writer;
    writer.setProject(edited);
    const QByteArray savedBytes = writer.saveToData();
    QVERIFY2(!savedBytes.isEmpty(), qPrintable(writer.errorString()));

    CompoundFile savedCf;
    QVERIFY(savedCf.openFromData(savedBytes));
    const int savedTimelineUid = timelineUid(savedCf);
    QCOMPARE(savedTimelineUid, sourceTimelineUid);
    BkndVarData savedViews;
    QVERIFY(savedViews.parse(
        savedCf.readStream({ QStringLiteral("   214"), QStringLiteral("CV_iew"), QStringLiteral("VarMeta") }),
        savedCf.readStream({ QStringLiteral("   214"), QStringLiteral("CV_iew"), QStringLiteral("Var2Data") })));
    QCOMPARE(savedViews.blobFor(quint32(savedTimelineUid), 47), sourceMembership);
}

void TstSemanticRoundtrip::realFixtures_data()
{
    QTest::addColumn<QString>("path");
    for (const QString &mpp : fixtures::mppFiles())
        QTest::newRow(qPrintable(fixtures::label(mpp))) << mpp;
}

void TstSemanticRoundtrip::realFixtures()
{
    // With no fixtures present this slot is still invoked once with no data row;
    // skip cleanly before touching QFETCH.
    if (fixtures::mppFiles().isEmpty())
        QSKIP("no .mpp fixtures present yet (add real files under tests/fixtures)");

    QFETCH(QString, path);

    QFile source(path);
    QVERIFY2(source.open(QIODevice::ReadOnly), qPrintable(source.errorString()));
    const QByteArray originalBytes = source.readAll();
    QVERIFY(!originalBytes.isEmpty());

    MppIO reader;
    QVERIFY2(reader.openFromData(originalBytes),
             qPrintable(QStringLiteral("%1: %2").arg(path, reader.errorString())));

    // An unchanged document must retain every opaque stream and every byte of
    // physical Compound File layout, not merely reconstruct the same model.
    const QByteArray unchangedBytes = reader.saveToData();
    QVERIFY2(!unchangedBytes.isEmpty(), qPrintable(reader.errorString()));
    QCOMPARE(unchangedBytes, originalBytes);

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
