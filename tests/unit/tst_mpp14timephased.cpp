// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/mpp14timephased.h"

#include <QTest>

namespace {

constexpr qint64 kHour = 3600LL * 1000LL;

schedule::TimephasedValue bucket(int type, const QDate &day, int hours)
{
    schedule::TimephasedValue value;
    value.type = type;
    value.uniqueId = 7;
    value.start = QDateTime(day, QTime(8, 0));
    value.finish = QDateTime(day, QTime(17, 0));
    value.unit = 1;
    value.value = QStringLiteral("PT%1H0M0S").arg(hours);
    return value;
}

qint64 total(const QList<schedule::TimephasedValue> &values, int type)
{
    qint64 result = 0;
    for (const auto &value : values)
        if (value.type == type)
            result += value.durationMillis();
    return result;
}

} // namespace

class TstMpp14Timephased : public QObject
{
    Q_OBJECT
private slots:
    void nonFlatWorkRoundTrips();
    void assignmentPeriodEditReconcilesAggregates();
    void actualOvertimeRoundTrips();
    void materialQuantityUsesProjectTimephasedStorage();
    void baselineBlobsMatchProject();
    void taskBaselineSumsAssignments();
};

void TstMpp14Timephased::nonFlatWorkRoundTrips()
{
    const schedule::WorkCalendar calendar;
    schedule::Assignment assignment;
    assignment.uniqueId = 7;
    assignment.start = QDateTime(QDate(2026, 8, 3), QTime(8, 0));
    assignment.finish = QDateTime(QDate(2026, 8, 6), QTime(17, 0));
    assignment.stop = QDateTime(QDate(2026, 8, 3), QTime(12, 0));
    assignment.resume = QDateTime(QDate(2026, 8, 4), QTime(8, 0));
    assignment.timephasedValues = {
        bucket(schedule::TimephasedValue::ActualWork, QDate(2026, 8, 3), 4),
        bucket(schedule::TimephasedValue::RemainingWork, QDate(2026, 8, 4), 6),
        bucket(schedule::TimephasedValue::RemainingWork, QDate(2026, 8, 5), 10)
    };
    // The actual bucket is a half day.
    assignment.timephasedValues[0].finish = assignment.stop;

    const QByteArray actualBlob =
        Mpp14Timephased::encodeActualWork(assignment.timephasedValues, calendar);
    const QByteArray remainingBlob =
        Mpp14Timephased::encodeRemainingWork(
            assignment.timephasedValues, calendar, QDateTime(), 3);
    QVERIFY(!actualBlob.isEmpty());
    QVERIFY(!remainingBlob.isEmpty());
    QCOMPARE(Mpp14Timephased::decodeWorkContour(remainingBlob), 3);

    const auto actual =
        Mpp14Timephased::decodeActualWork(actualBlob, assignment, calendar);
    const auto remaining =
        Mpp14Timephased::decodeRemainingWork(
            remainingBlob, assignment, calendar, actual);

    QCOMPARE(total(actual, schedule::TimephasedValue::ActualWork), 4 * kHour);
    QCOMPARE(total(remaining, schedule::TimephasedValue::RemainingWork), 16 * kHour);
    QCOMPARE(remaining.size(), 2);
    QCOMPARE(remaining.at(0).start.date(), QDate(2026, 8, 4));
    QCOMPARE(remaining.at(0).durationMillis(), 6 * kHour);
    QCOMPARE(remaining.at(1).start.date(), QDate(2026, 8, 5));
    QCOMPARE(remaining.at(1).durationMillis(), 10 * kHour);
}

void TstMpp14Timephased::assignmentPeriodEditReconcilesAggregates()
{
    schedule::Assignment assignment;
    assignment.uniqueId = 7;
    assignment.actualWorkMillis = 4 * kHour;
    assignment.remainingWorkMillis = 16 * kHour;
    assignment.workMillis = 20 * kHour;
    assignment.timephasedValues = {
        bucket(schedule::TimephasedValue::ActualWork, QDate(2026, 8, 3), 4),
        bucket(schedule::TimephasedValue::RemainingWork, QDate(2026, 8, 4), 6),
        bucket(schedule::TimephasedValue::RemainingWork, QDate(2026, 8, 5), 10)
    };

    const QDateTime from(QDate(2026, 8, 4), QTime(0, 0));
    const QDateTime to(QDate(2026, 8, 5), QTime(0, 0));
    QVERIFY(assignment.setTimephasedWorkInPeriod(
        schedule::TimephasedValue::RemainingWork, from, to, 8 * kHour));

    QCOMPARE(assignment.timephasedWorkInPeriod(
                 schedule::TimephasedValue::RemainingWork, from, to),
             8 * kHour);
    QCOMPARE(assignment.actualWorkMillis, 4 * kHour);
    QCOMPARE(assignment.remainingWorkMillis, 18 * kHour);
    QCOMPARE(assignment.workMillis, 22 * kHour);
    QCOMPARE(assignment.resume, from);

    QVERIFY(assignment.setTimephasedWorkInPeriod(
        schedule::TimephasedValue::ActualWork, from, to, 3 * kHour));
    QCOMPARE(assignment.actualWorkMillis, 7 * kHour);
    QCOMPARE(assignment.workMillis, 25 * kHour);
    QCOMPARE(assignment.stop, to);
}

void TstMpp14Timephased::actualOvertimeRoundTrips()
{
    const schedule::WorkCalendar calendar;
    schedule::Assignment assignment;
    assignment.uniqueId = 7;
    assignment.start = QDateTime(QDate(2026, 8, 3), QTime(8, 0));
    assignment.finish = QDateTime(QDate(2026, 8, 4), QTime(17, 0));
    assignment.timephasedValues = {
        bucket(schedule::TimephasedValue::ActualOvertimeWork,
               QDate(2026, 8, 3), 2),
        bucket(schedule::TimephasedValue::ActualOvertimeWork,
               QDate(2026, 8, 4), 3)
    };

    const QByteArray blob = Mpp14Timephased::encodeActualWork(
        assignment.timephasedValues, calendar, {},
        schedule::TimephasedValue::ActualOvertimeWork);
    QVERIFY(!blob.isEmpty());
    const auto decoded = Mpp14Timephased::decodeActualWork(
        blob, assignment, calendar,
        schedule::TimephasedValue::ActualOvertimeWork);
    QCOMPARE(total(decoded, schedule::TimephasedValue::ActualOvertimeWork),
             5 * kHour);
    QCOMPARE(decoded.size(), 2);
}

void TstMpp14Timephased::materialQuantityUsesProjectTimephasedStorage()
{
    schedule::Assignment assignment;
    assignment.uniqueId = 17;
    const QDateTime from(QDate(2026, 8, 4), QTime(0, 0));
    const QDateTime to(QDate(2026, 8, 5), QTime(0, 0));

    QVERIFY(assignment.setTimephasedMaterialInPeriod(
        schedule::TimephasedValue::RemainingWork, from, to, 7.25));
    QCOMPARE(assignment.timephasedMaterialInPeriod(
                 schedule::TimephasedValue::RemainingWork, from, to),
             7.25);
    QCOMPARE(assignment.remainingWorkMillis, qint64(7.25 * kHour));

    const schedule::WorkCalendar calendar;
    const QByteArray blob = Mpp14Timephased::encodeRemainingWork(
        assignment.timephasedValues, calendar);
    QVERIFY(!blob.isEmpty());
    assignment.start = from;
    assignment.finish = to;
    const auto decoded = Mpp14Timephased::decodeRemainingWork(
        blob, assignment, calendar, {});
    QCOMPARE(total(decoded, schedule::TimephasedValue::RemainingWork),
             qint64(7.25 * kHour));
}

// Ground truth: the assignment baseline blobs (keys 52/53) Project 2016 wrote
// for a 120 h assignment, 23 Sep 08:00 - 13 Oct 17:00 2026, $5280 at $44/h,
// after Project > Set Baseline. Our encoder must produce the same bytes
// (bar the cost blob's uninitialised marker words), or Project reads no
// baseline spread and shows BCWS/SV/CPI of 0 -- or, with the old 1984 epoch,
// every baseline day one day early.
void TstMpp14Timephased::baselineBlobsMatchProject()
{
    const schedule::WorkCalendar calendar;
    const QDateTime start(QDate(2026, 9, 23), QTime(8, 0));
    const QDateTime finish(QDate(2026, 10, 13), QTime(17, 0));
    schedule::TimephasedValue work;
    work.type = schedule::TimephasedValue::BaselineWork;
    work.start = start; work.finish = finish; work.unit = 1;
    work.value = QStringLiteral("PT120H0M0S");
    schedule::TimephasedValue cost = work;
    cost.type = schedule::TimephasedValue::BaselineCost;
    cost.value = QStringLiteral("5280");

    const QByteArray projectWork = QByteArray::fromHex(
        "030010001c0000000000000040775b41000000004019010000000000"
        "8dedb5a0f7c6b0be00000000010000008058650d"
        "0000000040775b41401901000000000098d2690d"
        "0000000040775b410000000001000000f7750a34");
    QCOMPARE(Mpp14Timephased::encodeBaselineWork({work}, 0, &calendar).toHex(),
             projectWork.toHex());

    QByteArray projectCost = QByteArray::fromHex(
        "03000400100000000000000000000000"
        "0100efc5530000008dedb5a0f7c6b0be8058650d"
        "0000efc55300000000000000001d204198d2690d"
        "010020000000000000000000001d2041f7750a34");
    // Project leaves garbage in the start/data marker words; we write 0.
    for (int record : {16, 36})
        projectCost.replace(record + 2, 4, QByteArray(4, '\0'));
    QCOMPARE(Mpp14Timephased::encodeBaselineCost({cost}, 0).toHex(),
             projectCost.toHex());

    const auto decodedWork = Mpp14Timephased::decodeBaselineWork(projectWork, 1, 0);
    QCOMPARE(decodedWork.size(), 1);
    QCOMPARE(decodedWork.at(0).start, start);
    QCOMPARE(decodedWork.at(0).finish, finish);
    QCOMPARE(decodedWork.at(0).durationMillis(), 120 * kHour);
    const auto decodedCost = Mpp14Timephased::decodeBaselineCost(projectCost, 1, 0);
    QCOMPARE(decodedCost.size(), 1);
    QCOMPARE(decodedCost.at(0).start, start);
    QCOMPARE(decodedCost.at(0).amount(), 5280.0);
}

void TstMpp14Timephased::taskBaselineSumsAssignments()
{
    // Two resources on the same days: the task spread is their per-period sum.
    QList<schedule::TimephasedValue> values;
    for (int resource = 0; resource < 2; ++resource)
        for (int day = 0; day < 2; ++day) {
            schedule::TimephasedValue value = bucket(
                schedule::TimephasedValue::BaselineCost, QDate(2026, 9, 22).addDays(day), 0);
            value.value = QStringLiteral("352");
            values.append(value);
        }
    const auto summed = Mpp14Timephased::sumBaselineValues(
        values, schedule::TimephasedValue::BaselineCost, 0, 42);
    QCOMPARE(summed.size(), 2);
    QCOMPARE(summed.at(0).amount(), 704.0);
    QCOMPARE(summed.at(1).amount(), 704.0);
    QCOMPARE(summed.at(1).uniqueId, 42);
}

QTEST_MAIN(TstMpp14Timephased)
#include "tst_mpp14timephased.moc"
