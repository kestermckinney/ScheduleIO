// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/progressupdating.h"

#include "mppio.h"
#include "xmlio.h"

#include "model/project.h"
#include "model/projectreconciliation.h"
#include "model/relation.h"
#include "model/resource.h"
#include "model/timephasedvalue.h"

#include "codec/fielddecoders.h"
#include "ole/compoundfile.h"

#include <QTest>
#include <QtEndian>

namespace {

constexpr qint64 kHour = 3600LL * 1000LL;

schedule::Task task(int uid, const QDateTime &start, qint64 duration = 8 * kHour)
{
    schedule::Task result;
    result.uniqueId = uid;
    result.id = uid;
    result.outlineLevel = 1;
    result.name = QStringLiteral("Task %1").arg(uid);
    result.start = start;
    result.finish = start.addMSecs(duration);
    result.durationMillis = duration;
    return result;
}

schedule::Assignment assignment(int uid, int taskUid, qint64 actual, qint64 remaining)
{
    schedule::Assignment result;
    result.uniqueId = uid;
    result.taskUniqueId = taskUid;
    result.resourceUniqueId = 1;
    result.units = 1.0;
    result.actualWorkMillis = actual;
    result.remainingWorkMillis = remaining;
    result.workMillis = actual + remaining;
    return result;
}

schedule::TimephasedValue bucket(int type, const QDateTime &start,
                                 const QDateTime &finish, int hours)
{
    schedule::TimephasedValue result;
    result.type = type;
    result.uniqueId = 1;
    result.start = start;
    result.finish = finish;
    result.unit = 1;
    result.value = QStringLiteral("PT%1H0M0S").arg(hours);
    return result;
}

schedule::Project projectWithWorkResource()
{
    schedule::Project project;
    project.scheduleFromStart = true;
    schedule::Resource resource;
    resource.uniqueId = 1;
    resource.id = 1;
    resource.name = QStringLiteral("Ann");
    resource.type = schedule::Resource::Type::Work;
    project.resources = { resource };
    return project;
}

// A task's STOP (u32@136) and RESUME (u32@132) as the MPP14 writer stored them,
// read straight from the TBkndTask fixed rows (47-byte FixedMeta items whose
// +4 is the row's FixedData offset; UNIQUE_ID at row offset 4).
QPair<QDateTime, QDateTime> writtenStopResume(const QByteArray &mpp, int uid)
{
    CompoundFile cf;
    if (!cf.openFromData(mpp))
        return {};
    const QStringList base{QStringLiteral("   114"), QStringLiteral("TBkndTask")};
    const QByteArray meta = cf.readStream(base + QStringList{QStringLiteral("FixedMeta")});
    const QByteArray data = cf.readStream(base + QStringList{QStringLiteral("FixedData")});
    const auto u32 = [](const QByteArray &b, int o) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData()) + o);
    };
    const int count = (meta.size() - 16) / 47;
    for (int i = 0; i < count; ++i) {
        const int at = int(u32(meta, 16 + i * 47 + 4));
        const int next = i + 1 < count ? int(u32(meta, 16 + (i + 1) * 47 + 4))
                                       : int(data.size());
        if (next - at < 140 || int(u32(data, at + 4)) != uid)
            continue;
        const QByteArray row = data.mid(at, next - at);
        return { FieldDecoders::decodeMppTimestamp(row, 136),
                 FieldDecoders::decodeMppTimestamp(row, 132) };
    }
    return {};
}

// A var-data blob of one entity row ("TBkndTask"/"TBkndAssn"), by field key.
QByteArray writtenVarBlob(const QByteArray &mpp, const QString &entity, int uid, quint16 key)
{
    CompoundFile cf;
    if (!cf.openFromData(mpp))
        return {};
    const QStringList base{QStringLiteral("   114"), entity};
    const QByteArray meta = cf.readStream(base + QStringList{QStringLiteral("VarMeta")});
    const QByteArray data = cf.readStream(base + QStringList{QStringLiteral("Var2Data")});
    const auto u32 = [](const QByteArray &b, int o) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData()) + o);
    };
    const int count = int(u32(meta, 8));
    for (int i = 0; i < count; ++i) {
        const int at = 24 + i * 12;
        if (int(u32(meta, at)) != uid
            || qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(meta.constData()) + at + 8) != key)
            continue;
        const int offset = int(u32(meta, at + 4));
        return data.mid(offset + 4, int(u32(data, offset)));
    }
    return {};
}

// Byte 10 of an assignment's 34-byte FixedMeta item (its progress-state flags).
int writtenAssignmentState(const QByteArray &mpp, int uid)
{
    CompoundFile cf;
    if (!cf.openFromData(mpp))
        return -1;
    const QStringList base{QStringLiteral("   114"), QStringLiteral("TBkndAssn")};
    const QByteArray meta = cf.readStream(base + QStringList{QStringLiteral("FixedMeta")});
    const QByteArray data = cf.readStream(base + QStringList{QStringLiteral("FixedData")});
    const int count = (meta.size() - 16) / 34;
    for (int i = 0; i < count; ++i) {
        const int at = int(qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(meta.constData()) + 16 + i * 34 + 4));
        if (at + 4 <= data.size()
            && int(qFromLittleEndian<quint32>(
                   reinterpret_cast<const uchar *>(data.constData()) + at)) == uid)
            return quint8(meta.at(16 + i * 34 + 10));
    }
    return -1;
}

} // namespace

class TstProgressUpdating : public QObject
{
    Q_OBJECT
private slots:
    void unstartedWorkMovesAfterStatusDate();
    void actualHistoryStaysAndRemainingBucketsMove();
    void inProgressTaskIsLeftAloneWhenSplitsDisabled();
    void moveOptionsPullSplitOntoStatusDate();
    void successorsFollowMovedFinish();
    void selectedScopeAndProtectedTasksAreHonored();
    void futureRemainingWorkIsNotPulledBackward();
    void scheduledPercentUsesWorkingTimeAndReconcilesAssignments();
    void zeroOrOneHundredAndSelectedScope();
    void percentWorkCompleteReconcilesWorkAssignmentsOnly();
    void directDurationAndDateFieldsStayCanonical();
    void directActualAndRemainingWorkReconcileAssignments();
    void scheduledProgressRoundTrips();
    void rescheduledProgressRoundTrips();
    void percentCompleteWritesCompleteThrough();
};

void TstProgressUpdating::directDurationAndDateFieldsStayCanonical()
{
    schedule::Project project = projectWithWorkResource();
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday, 40 * kHour) };
    project.tasks[0].finish = QDateTime(QDate(2026, 8, 7), QTime(17, 0));
    project.assignments = { assignment(1, 1, 0, 40 * kHour) };
    project.assignments[0].start = monday;
    project.assignments[0].finish = project.tasks[0].finish;

    QVERIFY(schedule::ProgressUpdating::setActualStart(
        project, 1, QDateTime(QDate(2026, 8, 4), QTime(8, 0))));
    QCOMPARE(project.tasks[0].start, project.tasks[0].actualStart);

    QVERIFY(schedule::ProgressUpdating::setActualDuration(project, 1, 16 * kHour));
    QCOMPARE(project.tasks[0].actualDurationMillis, 16 * kHour);
    QCOMPARE(project.tasks[0].durationMillis, 40 * kHour);
    QCOMPARE(project.tasks[0].percentComplete, 0.4);

    QVERIFY(schedule::ProgressUpdating::setRemainingDuration(project, 1, 8 * kHour));
    QCOMPARE(project.tasks[0].durationMillis, 24 * kHour);
    QCOMPARE(project.tasks[0].actualDurationMillis, 16 * kHour);
    QCOMPARE(project.tasks[0].percentComplete, 2.0 / 3.0);

    const QDateTime finish(QDate(2026, 8, 6), QTime(17, 0));
    QVERIFY(schedule::ProgressUpdating::setActualFinish(project, 1, finish));
    QCOMPARE(project.tasks[0].finish, finish);
    QCOMPARE(project.tasks[0].actualFinish, finish);
    QCOMPARE(project.tasks[0].percentComplete, 1.0);
    QCOMPARE(project.assignments[0].remainingWorkMillis, 0);
    QVERIFY(schedule::ProjectReconciliation::invariantViolations(project).isEmpty());
}

void TstProgressUpdating::directActualAndRemainingWorkReconcileAssignments()
{
    schedule::Project project = projectWithWorkResource();
    schedule::Resource second = project.resources.first();
    second.uniqueId = 2;
    second.id = 2;
    project.resources.append(second);
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday, 40 * kHour) };
    project.tasks[0].finish = QDateTime(QDate(2026, 8, 7), QTime(17, 0));
    project.assignments = { assignment(1, 1, 0, 8 * kHour),
                            assignment(2, 1, 0, 32 * kHour) };
    project.assignments[1].resourceUniqueId = 2;
    for (schedule::Assignment &a : project.assignments) {
        a.start = monday;
        a.finish = project.tasks[0].finish;
    }

    QVERIFY(schedule::ProgressUpdating::setActualWork(project, 1, 10 * kHour));
    QCOMPARE(project.tasks[0].actualWorkMillis, 10 * kHour);
    QCOMPARE(project.assignments[0].actualWorkMillis, 2 * kHour);
    QCOMPARE(project.assignments[1].actualWorkMillis, 8 * kHour);

    QVERIFY(schedule::ProgressUpdating::setRemainingWork(project, 1, 20 * kHour));
    QCOMPARE(project.tasks[0].actualWorkMillis, 10 * kHour);
    QCOMPARE(project.tasks[0].workMillis, 30 * kHour);
    QCOMPARE(project.assignments[0].workMillis, 6 * kHour);
    QCOMPARE(project.assignments[1].workMillis, 24 * kHour);
    QCOMPARE(project.tasks[0].percentComplete, 1.0 / 3.0);
    QVERIFY(schedule::ProjectReconciliation::invariantViolations(project).isEmpty());
}

void TstProgressUpdating::unstartedWorkMovesAfterStatusDate()
{
    schedule::Project project = projectWithWorkResource();
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday) };
    project.assignments = { assignment(1, 1, 0, 8 * kHour) };
    project.assignments[0].start = monday;
    project.assignments[0].finish = QDateTime(QDate(2026, 8, 3), QTime(17, 0));

    QCOMPARE(schedule::ProgressUpdating::rescheduleIncompleteWork(
                 project, QDateTime(QDate(2026, 8, 7), QTime(17, 0))), 1);

    const QDateTime nextMonday(QDate(2026, 8, 10), QTime(8, 0));
    QCOMPARE(project.tasks[0].start, nextMonday);
    QCOMPARE(project.tasks[0].finish, QDateTime(QDate(2026, 8, 10), QTime(17, 0)));
    QCOMPARE(project.assignments[0].start, nextMonday);
    QVERIFY(!project.tasks[0].actualStart.isValid());
    QVERIFY(!project.assignments[0].stop.isValid());
}

void TstProgressUpdating::actualHistoryStaysAndRemainingBucketsMove()
{
    schedule::Project project = projectWithWorkResource();
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday, 16 * kHour) };
    project.tasks[0].actualStart = monday;
    project.tasks[0].actualDurationMillis = 4 * kHour;
    project.tasks[0].actualWorkMillis = 4 * kHour;
    project.tasks[0].percentComplete = 0.25;
    project.assignments = { assignment(1, 1, 4 * kHour, 12 * kHour) };
    schedule::Assignment &a = project.assignments[0];
    a.start = monday;
    a.finish = QDateTime(QDate(2026, 8, 4), QTime(17, 0));
    a.timephasedValues = {
        bucket(schedule::TimephasedValue::ActualWork, monday,
               QDateTime(QDate(2026, 8, 3), QTime(12, 0)), 4),
        bucket(schedule::TimephasedValue::RemainingWork,
               QDateTime(QDate(2026, 8, 3), QTime(13, 0)),
               QDateTime(QDate(2026, 8, 3), QTime(17, 0)), 4),
        bucket(schedule::TimephasedValue::RemainingWork,
               QDateTime(QDate(2026, 8, 4), QTime(8, 0)),
               QDateTime(QDate(2026, 8, 4), QTime(17, 0)), 8)
    };

    QCOMPARE(schedule::ProgressUpdating::rescheduleIncompleteWork(
                 project, QDateTime(QDate(2026, 8, 5), QTime(17, 0))), 1);

    QCOMPARE(project.tasks[0].start, monday);
    QCOMPARE(project.tasks[0].actualStart, monday);
    QCOMPARE(a.stop, QDateTime(QDate(2026, 8, 3), QTime(12, 0)));
    QCOMPARE(a.resume, QDateTime(QDate(2026, 8, 6), QTime(8, 0)));
    QCOMPARE(a.timephasedValues[0].start, monday); // actual bucket untouched
    QCOMPARE(a.timephasedValues[1].start, a.resume);
    QCOMPARE(a.timephasedValues[2].start, QDateTime(QDate(2026, 8, 6), QTime(13, 0)));
    QCOMPARE(a.finish, QDateTime(QDate(2026, 8, 7), QTime(12, 0)));
    QCOMPARE(project.tasks[0].finish, a.finish);
    QCOMPARE(a.actualWorkMillis, 4 * kHour);
    QCOMPARE(a.remainingWorkMillis, 12 * kHour);
}

void TstProgressUpdating::inProgressTaskIsLeftAloneWhenSplitsDisabled()
{
    schedule::Project project = projectWithWorkResource();
    project.splitInProgressTasks = false;   // File > Options > Schedule
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday, 16 * kHour), task(2, monday, 8 * kHour) };
    project.tasks[0].actualStart = monday;
    project.tasks[0].actualDurationMillis = 4 * kHour;
    project.tasks[0].actualWorkMillis = 4 * kHour;
    project.tasks[0].percentComplete = 0.25;
    project.assignments = { assignment(1, 1, 4 * kHour, 12 * kHour),
                            assignment(2, 2, 0, 8 * kHour) };
    schedule::Assignment &a = project.assignments[0];
    a.start = monday;
    a.finish = QDateTime(QDate(2026, 8, 4), QTime(17, 0));
    project.assignments[1].start = monday;
    project.assignments[1].finish = QDateTime(QDate(2026, 8, 3), QTime(17, 0));

    // Only the unstarted task (2) is rescheduled; the in-progress task keeps its
    // plan and is never split.
    QCOMPARE(schedule::ProgressUpdating::rescheduleIncompleteWork(
                 project, QDateTime(QDate(2026, 8, 5), QTime(17, 0))), 1);
    QCOMPARE(project.tasks[0].start, monday);
    // Not pushed past the reschedule boundary, and never split.
    QVERIFY(project.tasks[0].finish.date() <= QDate(2026, 8, 4));
    QVERIFY(!a.stop.isValid());
    QVERIFY(!a.resume.isValid());
    QVERIFY(project.tasks[1].start >= QDateTime(QDate(2026, 8, 6), QTime(0, 0)));
}

void TstProgressUpdating::moveOptionsPullSplitOntoStatusDate()
{
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    const QDateTime status(QDate(2026, 8, 5), QTime(17, 0));   // Wed
    auto freshProject = [&] {
        schedule::Project project = projectWithWorkResource();
        project.tasks = { task(1, monday, 40 * kHour) };
        project.tasks[0].finish = QDateTime(QDate(2026, 8, 7), QTime(17, 0));
        project.assignments = { assignment(1, 1, 0, 40 * kHour) };
        project.assignments[0].start = monday;
        project.assignments[0].finish = project.tasks[0].finish;
        project.statusDate = status;
        return project;
    };

    // --- Case B: remaining work would be scheduled before the status date. ---
    {
        schedule::Project p = freshProject();   // all move* options off (default)
        QVERIFY(schedule::ProgressUpdating::setPercentWorkComplete(p, 1, 0.25));
        // Split falls ~Tue, i.e. before the Wed status date, and stays there.
        QVERIFY(p.assignments[0].resume.isValid());
        QVERIFY(p.assignments[0].resume < status);
    }
    {
        schedule::Project p = freshProject();
        p.moveRemainingStartsForward = true;
        QVERIFY(schedule::ProgressUpdating::setPercentWorkComplete(p, 1, 0.25));
        QCOMPARE(p.assignments[0].resume, status);          // remaining pushed forward
        QVERIFY(p.assignments[0].stop < status);            // completed part left put
    }
    {
        schedule::Project p = freshProject();
        p.moveRemainingStartsForward = true;
        p.moveCompletedEndsForward = true;
        QVERIFY(schedule::ProgressUpdating::setPercentWorkComplete(p, 1, 0.25));
        QCOMPARE(p.assignments[0].resume, status);
        QCOMPARE(p.assignments[0].stop, status);            // completed part follows
    }

    // --- Case A: completed work would reach past the status date. ---
    {
        schedule::Project p = freshProject();
        QVERIFY(schedule::ProgressUpdating::setPercentWorkComplete(p, 1, 0.75));
        QVERIFY(p.assignments[0].stop > status);            // default: left past status
    }
    {
        schedule::Project p = freshProject();
        p.moveCompletedEndsBack = true;
        QVERIFY(schedule::ProgressUpdating::setPercentWorkComplete(p, 1, 0.75));
        QCOMPARE(p.assignments[0].stop, status);            // completed end pulled back
        QVERIFY(p.assignments[0].resume > status);          // gap: remaining left put
    }
    {
        schedule::Project p = freshProject();
        p.moveCompletedEndsBack = true;
        p.moveRemainingStartsBack = true;
        QVERIFY(schedule::ProgressUpdating::setPercentWorkComplete(p, 1, 0.75));
        QCOMPARE(p.assignments[0].stop, status);
        QCOMPARE(p.assignments[0].resume, status);          // gap closed
    }
}

void TstProgressUpdating::successorsFollowMovedFinish()
{
    schedule::Project project = projectWithWorkResource();
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday), task(2, QDateTime(QDate(2026, 8, 4), QTime(8, 0))) };
    project.assignments = { assignment(1, 1, 0, 8 * kHour) };
    project.assignments[0].start = monday;
    project.assignments[0].finish = QDateTime(QDate(2026, 8, 3), QTime(17, 0));
    schedule::Relation relation;
    relation.uniqueId = 1;
    relation.predecessorTaskUid = 1;
    relation.successorTaskUid = 2;
    relation.type = schedule::Relation::FinishToStart;
    project.relations = { relation };

    QCOMPARE(schedule::ProgressUpdating::rescheduleIncompleteWork(
                 project, QDateTime(QDate(2026, 8, 5), QTime(17, 0)), { 1 }), 1);
    QCOMPARE(project.tasks[0].finish, QDateTime(QDate(2026, 8, 6), QTime(17, 0)));
    QCOMPARE(project.tasks[1].start, QDateTime(QDate(2026, 8, 7), QTime(8, 0)));
}

void TstProgressUpdating::selectedScopeAndProtectedTasksAreHonored()
{
    schedule::Project project = projectWithWorkResource();
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday), task(2, monday), task(3, monday), task(4, monday) };
    project.tasks[1].manual = true;
    project.tasks[2].active = false;
    project.tasks[3].actualFinish = QDateTime(QDate(2026, 8, 3), QTime(17, 0));

    QCOMPARE(schedule::ProgressUpdating::rescheduleIncompleteWork(
                 project, QDateTime(QDate(2026, 8, 5), QTime(17, 0)), { 1, 2, 3, 4 }), 1);
    QCOMPARE(project.tasks[0].start, QDateTime(QDate(2026, 8, 6), QTime(8, 0)));
    QCOMPARE(project.tasks[1].start, monday);
    QCOMPARE(project.tasks[2].start, monday);
    QCOMPARE(project.tasks[3].start, monday);
}

void TstProgressUpdating::futureRemainingWorkIsNotPulledBackward()
{
    schedule::Project project = projectWithWorkResource();
    const QDateTime future(QDate(2026, 8, 10), QTime(8, 0));
    project.tasks = { task(1, future) };
    project.assignments = { assignment(1, 1, 0, 8 * kHour) };
    project.assignments[0].start = future;
    project.assignments[0].finish = QDateTime(QDate(2026, 8, 10), QTime(17, 0));

    QCOMPARE(schedule::ProgressUpdating::rescheduleIncompleteWork(
                 project, QDateTime(QDate(2026, 8, 5), QTime(17, 0))), 0);
    QCOMPARE(project.tasks[0].start, future);
    QCOMPARE(project.assignments[0].start, future);
}

void TstProgressUpdating::scheduledPercentUsesWorkingTimeAndReconcilesAssignments()
{
    schedule::Project project = projectWithWorkResource();
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday, 40 * kHour) };
    project.tasks[0].finish = QDateTime(QDate(2026, 8, 7), QTime(17, 0));
    project.assignments = { assignment(1, 1, 0, 40 * kHour) };
    project.assignments[0].start = monday;
    project.assignments[0].finish = project.tasks[0].finish;

    QCOMPARE(schedule::ProgressUpdating::updateScheduledProgress(
                 project, QDateTime(QDate(2026, 8, 5), QTime(17, 0)),
                 schedule::ProgressUpdating::UpdateAction::ScheduledPercent), 1);

    QCOMPARE(project.tasks[0].percentComplete, 0.6);
    QCOMPARE(project.tasks[0].actualStart, monday);
    QVERIFY(!project.tasks[0].actualFinish.isValid());
    QCOMPARE(project.tasks[0].actualDurationMillis, 24 * kHour);
    QCOMPARE(project.assignments[0].actualWorkMillis, 24 * kHour);
    QCOMPARE(project.assignments[0].remainingWorkMillis, 16 * kHour);
    QCOMPARE(project.assignments[0].timephasedValues.size(), 2);
    QVERIFY(schedule::ProjectReconciliation::invariantViolations(project).isEmpty());
}

void TstProgressUpdating::zeroOrOneHundredAndSelectedScope()
{
    schedule::Project project = projectWithWorkResource();
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday), task(2, monday, 40 * kHour), task(3, monday) };
    project.tasks[0].finish = QDateTime(QDate(2026, 8, 3), QTime(17, 0));
    project.tasks[1].finish = QDateTime(QDate(2026, 8, 7), QTime(17, 0));
    project.tasks[1].percentComplete = 0.5;
    project.tasks[2].active = false;

    QCOMPARE(schedule::ProgressUpdating::updateScheduledProgress(
                 project, QDateTime(QDate(2026, 8, 5), QTime(17, 0)),
                 schedule::ProgressUpdating::UpdateAction::ZeroOrOneHundred,
                 { 1, 2, 3 }), 1);

    QCOMPARE(project.tasks[0].percentComplete, 1.0);
    QCOMPARE(project.tasks[0].actualFinish, project.tasks[0].finish);
    QCOMPARE(project.tasks[1].percentComplete, 0.5);
    QVERIFY(!project.tasks[1].actualStart.isValid());
    QCOMPARE(project.tasks[2].percentComplete, 0.0);
}

void TstProgressUpdating::percentWorkCompleteReconcilesWorkAssignmentsOnly()
{
    schedule::Project project = projectWithWorkResource();
    schedule::Resource material;
    material.uniqueId = 2;
    material.id = 2;
    material.name = QStringLiteral("Steel");
    material.type = schedule::Resource::Type::Material;
    project.resources.append(material);
    schedule::Resource secondWorker;
    secondWorker.uniqueId = 3;
    secondWorker.id = 3;
    secondWorker.name = QStringLiteral("Ben");
    secondWorker.type = schedule::Resource::Type::Work;
    project.resources.append(secondWorker);
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday, 40 * kHour) };
    project.tasks[0].finish = QDateTime(QDate(2026, 8, 7), QTime(17, 0));
    project.assignments = { assignment(1, 1, 0, 8 * kHour),
                            assignment(3, 1, 0, 32 * kHour) };
    project.assignments[0].start = monday;
    project.assignments[0].finish = project.tasks[0].finish;
    project.assignments[1].resourceUniqueId = 3;
    project.assignments[1].start = monday;
    project.assignments[1].finish = project.tasks[0].finish;
    schedule::Assignment materialAssignment = assignment(2, 1, 0, 10 * kHour);
    materialAssignment.resourceUniqueId = 2;
    materialAssignment.start = monday;
    materialAssignment.finish = project.tasks[0].finish;
    project.assignments.append(materialAssignment);
    project.statusDate = QDateTime(QDate(2026, 8, 5), QTime(17, 0));

    QVERIFY(schedule::ProgressUpdating::setPercentWorkComplete(project, 1, 0.25));
    QCOMPARE(project.tasks[0].actualWorkMillis, 10 * kHour);
    QCOMPARE(project.tasks[0].percentComplete, 0.25);
    QCOMPARE(project.tasks[0].actualDurationMillis, 10 * kHour);
    QCOMPARE(project.tasks[0].actualStart, monday);
    QVERIFY(!project.tasks[0].actualFinish.isValid());
    QCOMPARE(project.assignments[0].actualWorkMillis, 2 * kHour);
    QCOMPARE(project.assignments[0].remainingWorkMillis, 6 * kHour);
    QCOMPARE(project.assignments[1].actualWorkMillis, 8 * kHour);
    QCOMPARE(project.assignments[1].remainingWorkMillis, 24 * kHour);
    QCOMPARE(project.assignments[2].actualWorkMillis, 0);
    QCOMPARE(project.assignments[2].remainingWorkMillis, 10 * kHour);
    QVERIFY(schedule::ProjectReconciliation::invariantViolations(project).isEmpty());

    auto verifyPartial = [](const schedule::Project &decoded) {
        QCOMPARE(decoded.tasks.size(), 1);
        QCOMPARE(decoded.tasks[0].percentComplete, 0.25);
        QCOMPARE(decoded.tasks[0].actualDurationMillis, 10 * kHour);
        QVERIFY(decoded.tasks[0].actualStart.isValid());
        QVERIFY(!decoded.tasks[0].actualFinish.isValid());
        QCOMPARE(decoded.tasks[0].actualWorkMillis, 10 * kHour);
        QCOMPARE(decoded.assignments[0].actualWorkMillis, 2 * kHour);
        QCOMPARE(decoded.assignments[0].remainingWorkMillis, 6 * kHour);
        QCOMPARE(decoded.assignments[1].actualWorkMillis, 8 * kHour);
        QCOMPARE(decoded.assignments[1].remainingWorkMillis, 24 * kHour);
        QCOMPARE(decoded.assignments[2].actualWorkMillis, 0);
        QCOMPARE(decoded.assignments[2].remainingWorkMillis, 10 * kHour);
    };
    MppIO mppWriter;
    mppWriter.setProject(project);
    const QByteArray mpp = mppWriter.saveToData();
    QVERIFY2(!mpp.isEmpty(), qPrintable(mppWriter.errorString()));
    MppIO mppReader;
    QVERIFY2(mppReader.openFromData(mpp), qPrintable(mppReader.errorString()));
    verifyPartial(mppReader.project());

    XmlIO xmlWriter;
    xmlWriter.setProject(project);
    const QByteArray xml = xmlWriter.saveToData();
    QVERIFY2(!xml.isEmpty(), qPrintable(xmlWriter.errorString()));
    XmlIO xmlReader;
    QVERIFY2(xmlReader.openFromData(xml), qPrintable(xmlReader.errorString()));
    verifyPartial(xmlReader.project());

    QVERIFY(schedule::ProgressUpdating::setPercentWorkComplete(project, 1, 1.0));
    QCOMPARE(project.tasks[0].actualWorkMillis, 40 * kHour);
    QCOMPARE(project.tasks[0].percentComplete, 1.0);
    QCOMPARE(project.tasks[0].actualDurationMillis, 40 * kHour);
    QCOMPARE(project.tasks[0].actualFinish, project.tasks[0].finish);
    QCOMPARE(project.assignments[0].remainingWorkMillis, 0);
    QCOMPARE(project.assignments[1].remainingWorkMillis, 0);
}

void TstProgressUpdating::scheduledProgressRoundTrips()
{
    schedule::Project project = projectWithWorkResource();
    project.formatVersion = schedule::Project::FormatVersion::Mpp14;
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday, 40 * kHour) };
    project.tasks[0].finish = QDateTime(QDate(2026, 8, 7), QTime(17, 0));
    project.assignments = { assignment(1, 1, 0, 40 * kHour) };
    project.assignments[0].start = monday;
    project.assignments[0].finish = project.tasks[0].finish;
    project.statusDate = QDateTime(QDate(2026, 8, 5), QTime(17, 0));
    QCOMPARE(schedule::ProgressUpdating::updateScheduledProgress(
                 project, project.statusDate,
                 schedule::ProgressUpdating::UpdateAction::ScheduledPercent), 1);

    auto verify = [](const schedule::Project &decoded) {
        QCOMPARE(decoded.tasks.size(), 1);
        QCOMPARE(decoded.assignments.size(), 1);
        QCOMPARE(decoded.tasks[0].percentComplete, 0.6);
        QCOMPARE(decoded.tasks[0].actualDurationMillis, 24 * kHour);
        QCOMPARE(decoded.assignments[0].actualWorkMillis, 24 * kHour);
        QCOMPARE(decoded.assignments[0].remainingWorkMillis, 16 * kHour);
    };
    MppIO mppWriter;
    mppWriter.setProject(project);
    const QByteArray mpp = mppWriter.saveToData();
    QVERIFY2(!mpp.isEmpty(), qPrintable(mppWriter.errorString()));
    MppIO mppReader;
    QVERIFY2(mppReader.openFromData(mpp), qPrintable(mppReader.errorString()));
    verify(mppReader.project());

    XmlIO xmlWriter;
    xmlWriter.setProject(project);
    const QByteArray xml = xmlWriter.saveToData();
    QVERIFY2(!xml.isEmpty(), qPrintable(xmlWriter.errorString()));
    XmlIO xmlReader;
    QVERIFY2(xmlReader.openFromData(xml), qPrintable(xmlReader.errorString()));
    verify(xmlReader.project());
}

void TstProgressUpdating::rescheduledProgressRoundTrips()
{
    schedule::Project project = projectWithWorkResource();
    project.formatVersion = schedule::Project::FormatVersion::Mpp14;
    project.statusDate = QDateTime(QDate(2026, 8, 5), QTime(17, 0));
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday, 16 * kHour) };
    project.tasks[0].actualStart = monday;
    project.tasks[0].actualDurationMillis = 4 * kHour;
    project.tasks[0].actualWorkMillis = 4 * kHour;
    project.tasks[0].percentComplete = 0.25;
    project.assignments = { assignment(1, 1, 4 * kHour, 12 * kHour) };
    schedule::Assignment &assignment = project.assignments[0];
    assignment.start = monday;
    assignment.finish = QDateTime(QDate(2026, 8, 4), QTime(17, 0));
    assignment.timephasedValues = {
        bucket(schedule::TimephasedValue::ActualWork, monday,
               QDateTime(QDate(2026, 8, 3), QTime(12, 0)), 4),
        bucket(schedule::TimephasedValue::RemainingWork,
               QDateTime(QDate(2026, 8, 4), QTime(8, 0)),
               QDateTime(QDate(2026, 8, 5), QTime(17, 0)), 12)
    };
    QCOMPARE(schedule::ProgressUpdating::rescheduleIncompleteWork(
                 project, project.statusDate), 1);

    auto verify = [&](const schedule::Project &decoded) {
        QCOMPARE(decoded.statusDate.date(), project.statusDate.date());
        QCOMPARE(decoded.statusDate.time(), project.statusDate.time());
        QCOMPARE(decoded.tasks.size(), 1);
        QCOMPARE(decoded.assignments.size(), 1);
        QCOMPARE(decoded.tasks[0].finish.date(), project.tasks[0].finish.date());
        QCOMPARE(decoded.assignments[0].stop.date(), project.assignments[0].stop.date());
        QCOMPARE(decoded.assignments[0].stop.time(), project.assignments[0].stop.time());
        QCOMPARE(decoded.assignments[0].resume.date(), project.assignments[0].resume.date());
        QCOMPARE(decoded.assignments[0].resume.time(), project.assignments[0].resume.time());
        QCOMPARE(decoded.assignments[0].actualWorkMillis, 4 * kHour);
        QCOMPARE(decoded.assignments[0].remainingWorkMillis, 12 * kHour);
    };

    MppIO mppWriter;
    mppWriter.setProject(project);
    const QByteArray mpp = mppWriter.saveToData();
    QVERIFY2(!mpp.isEmpty(), qPrintable(mppWriter.errorString()));
    MppIO mppReader;
    QVERIFY2(mppReader.openFromData(mpp), qPrintable(mppReader.errorString()));
    verify(mppReader.project());

    XmlIO xmlWriter;
    xmlWriter.setProject(project);
    const QByteArray xml = xmlWriter.saveToData();
    QVERIFY2(!xml.isEmpty(), qPrintable(xmlWriter.errorString()));
    XmlIO xmlReader;
    QVERIFY2(xmlReader.openFromData(xml), qPrintable(xmlReader.errorString()));
    verify(xmlReader.project());
}

// Project draws the Progress bar to Complete Through, which it takes from the
// task's Stop/Resume. Left at NA on a started task, Project reads them as the
// Finish and an 8%-complete task opens with a full progress bar.
void TstProgressUpdating::percentCompleteWritesCompleteThrough()
{
    schedule::Project project = projectWithWorkResource();
    project.formatVersion = schedule::Project::FormatVersion::Mpp14;
    const QDateTime monday(QDate(2026, 8, 3), QTime(8, 0));
    project.tasks = { task(1, monday, 40 * kHour), task(2, monday, 40 * kHour),
                      task(3, monday, 40 * kHour) };
    for (schedule::Task &t : project.tasks)
        t.finish = QDateTime(QDate(2026, 8, 7), QTime(17, 0));
    project.assignments = { assignment(1, 1, 0, 40 * kHour) };
    project.assignments[0].start = monday;
    project.assignments[0].finish = project.tasks[0].finish;
    QVERIFY(schedule::ProgressUpdating::setPercentComplete(project, 1, 0.25));
    QVERIFY(schedule::ProgressUpdating::setPercentComplete(project, 2, 1.0));

    MppIO writer;
    writer.setProject(project);
    const QByteArray mpp = writer.saveToData();
    QVERIFY2(!mpp.isEmpty(), qPrintable(writer.errorString()));

    // 25% of 40h = 10h: Monday's 8h, then 08:00-10:00 on Tuesday.
    const auto partial = writtenStopResume(mpp, 1);
    QCOMPARE(partial.first, QDateTime(QDate(2026, 8, 4), QTime(10, 0)));
    QCOMPARE(partial.second, QDateTime(QDate(2026, 8, 4), QTime(10, 0)));
    const auto complete = writtenStopResume(mpp, 2);
    QCOMPARE(complete.first, QDateTime(QDate(2026, 8, 7), QTime(17, 0)));
    QCOMPARE(complete.second, QDateTime(QDate(2026, 8, 7), QTime(17, 0)));
    const auto unstarted = writtenStopResume(mpp, 3);
    QVERIFY(!unstarted.first.isValid());
    QVERIFY(!unstarted.second.isValid());

    // Project reads Complete Through from the task's %-complete curve (var
    // key 350): Actual Start, then the Stop date at 25%, then a fixed closing
    // point. Dates are tenths of a minute since 1983-12-31.
    const auto tenths = [](const QDateTime &dt) {
        return quint32(QDate(1983, 12, 31).daysTo(dt.date()) * 14400
                       + dt.time().msecsSinceStartOfDay() / 6000);
    };
    QByteArray curve = QByteArray::fromHex("03000400100000000000000000000000");
    const auto point = [&curve](quint32 kind, double value, quint32 date) {
        uchar b[20] = {};
        qToLittleEndian<quint32>(kind, b);
        quint64 bits;
        memcpy(&bits, &value, 8);
        qToLittleEndian<quint64>(bits, b + 8);
        qToLittleEndian<quint32>(date, b + 16);
        curve.append(reinterpret_cast<const char *>(b), 20);
    };
    point(1, 32768.0, tenths(monday));
    point(0, 25.0, tenths(QDateTime(QDate(2026, 8, 4), QTime(10, 0))));
    point(1, 25.0, 0x340A75F7u);
    QCOMPARE(writtenVarBlob(mpp, QStringLiteral("TBkndTask"), 1, 350).toHex(), curve.toHex());
    QVERIFY(writtenVarBlob(mpp, QStringLiteral("TBkndTask"), 3, 350).isEmpty());

    // The in-progress assignment carries Project's 0x2B state, not the
    // completed rows' 0x3B (which makes Project draw the work as finished).
    QCOMPARE(writtenAssignmentState(mpp, 1), 0x2B);
}

QTEST_MAIN(TstProgressUpdating)
#include "tst_progressupdating.moc"
