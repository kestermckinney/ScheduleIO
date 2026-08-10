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
    const QDateTime from(QDate(2026, 8, 4), QTime(0, 0), Qt::UTC);
    const QDateTime to(QDate(2026, 8, 5), QTime(0, 0), Qt::UTC);

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

QTEST_MAIN(TstMpp14Timephased)
#include "tst_mpp14timephased.moc"
