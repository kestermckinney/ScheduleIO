// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/mpp14timephased.h"

#include "codec/fielddecoders.h"

#include <QtEndian>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr qint64 kMinuteMs = 60LL * 1000LL;
constexpr qint64 kHourMs = 60LL * kMinuteMs;

void appendU16(QByteArray &bytes, quint16 value)
{
    char raw[2];
    qToLittleEndian(value, reinterpret_cast<uchar *>(raw));
    bytes.append(raw, 2);
}

void appendU32(QByteArray &bytes, quint32 value)
{
    char raw[4];
    qToLittleEndian(value, reinterpret_cast<uchar *>(raw));
    bytes.append(raw, 4);
}

void appendDouble(QByteArray &bytes, double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(value));
    char raw[8];
    qToLittleEndian(bits, reinterpret_cast<uchar *>(raw));
    bytes.append(raw, 8);
}

void pokeU16(QByteArray &bytes, int offset, quint16 value)
{
    if (offset >= 0 && offset + 2 <= bytes.size())
        qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + offset));
}

void pokeU32(QByteArray &bytes, int offset, quint32 value)
{
    if (offset >= 0 && offset + 4 <= bytes.size())
        qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + offset));
}

void pokeDouble(QByteArray &bytes, int offset, double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(value));
    if (offset >= 0 && offset + 8 <= bytes.size())
        qToLittleEndian(bits, reinterpret_cast<uchar *>(bytes.data() + offset));
}

QString isoDuration(qint64 millis)
{
    const bool negative = millis < 0;
    quint64 seconds = quint64(negative ? -millis : millis) / 1000;
    const quint64 hours = seconds / 3600;
    seconds %= 3600;
    const quint64 minutes = seconds / 60;
    seconds %= 60;
    return QStringLiteral("%1PT%2H%3M%4S")
        .arg(negative ? QStringLiteral("-") : QString())
        .arg(hours).arg(minutes).arg(seconds);
}

void appendDaily(QList<schedule::TimephasedValue> &out, int type, int assignmentUid,
                 const QDateTime &start, const QDateTime &finish, qint64 workMillis,
                 const schedule::WorkCalendar &calendar)
{
    if (!start.isValid() || !finish.isValid() || finish <= start || workMillis <= 0)
        return;

    qint64 basis = calendar.workBetween(start, finish);
    const bool useWallClock = basis <= 0;
    if (useWallClock)
        basis = start.msecsTo(finish);
    if (basis <= 0)
        return;

    qint64 allocated = 0;
    struct Slice { QDateTime start; QDateTime finish; qint64 basis = 0; };
    QList<Slice> slices;
    for (QDate day = start.date(); day <= finish.date(); day = day.addDays(1)) {
        const QDateTime dayStart(day, QTime(0, 0), start.timeZone());
        const QDateTime dayFinish(day.addDays(1), QTime(0, 0), start.timeZone());
        const QDateTime a = qMax(start, dayStart);
        const QDateTime b = qMin(finish, dayFinish);
        if (b <= a)
            continue;
        const qint64 sliceBasis = useWallClock ? a.msecsTo(b)
                                              : calendar.workBetween(a, b);
        if (sliceBasis > 0) {
            const QDateTime sliceStart = useWallClock ? a : calendar.nextWorkStart(a);
            const QDateTime sliceFinish = useWallClock
                ? b : calendar.addWork(sliceStart, sliceBasis);
            slices.append({ sliceStart, sliceFinish, sliceBasis });
        }
    }

    for (int i = 0; i < slices.size(); ++i) {
        const Slice &slice = slices.at(i);
        const qint64 amount = i + 1 == slices.size()
            ? workMillis - allocated
            : qint64(std::llround(double(workMillis) * double(slice.basis) / double(basis)));
        allocated += amount;
        schedule::TimephasedValue value;
        value.type = type;
        value.uniqueId = assignmentUid;
        value.start = slice.start;
        value.finish = slice.finish;
        value.unit = 1;
        value.value = isoDuration(amount);
        out.append(value);
    }
}

QList<schedule::TimephasedValue> sortedValues(
    const QList<schedule::TimephasedValue> &all, int type)
{
    QList<schedule::TimephasedValue> values;
    for (const schedule::TimephasedValue &value : all)
        if (value.type == type && value.durationMillis() > 0
            && value.start.isValid() && value.finish > value.start)
            values.append(value);
    std::sort(values.begin(), values.end(), [](const auto &left, const auto &right) {
        return left.start < right.start;
    });
    return values;
}

QList<schedule::TimephasedValue> sortedBaselineValues(
    const QList<schedule::TimephasedValue> &all, int type, int baselineNumber)
{
    QList<schedule::TimephasedValue> values;
    for (const schedule::TimephasedValue &value : all)
        if (value.type == type && value.baselineNumber == baselineNumber
            && value.start.isValid() && value.finish > value.start)
            values.append(value);
    std::sort(values.begin(), values.end(), [](const auto &left, const auto &right) {
        return left.start < right.start;
    });
    return values;
}

qint64 elapsedMillis(const schedule::WorkCalendar &calendar,
                     const QDateTime &anchor, const QDateTime &finish)
{
    qint64 elapsed = calendar.workBetween(anchor, finish);
    if (elapsed <= 0)
        elapsed = anchor.msecsTo(finish);
    return qMax<qint64>(0, elapsed);
}

double plannedRate(const schedule::TimephasedValue &value,
                   const schedule::WorkCalendar &calendar)
{
    const qint64 elapsed = qMax<qint64>(
        1, elapsedMillis(calendar, value.start, value.finish));
    const double workingDays = qMax(1.0, double(elapsed) / double(calendar.workPerDay()));
    return (double(value.durationMillis()) / double(kHourMs) / workingDays) * 20000.0;
}

double actualRate(const schedule::TimephasedValue &value,
                  const schedule::WorkCalendar &calendar)
{
    const qint64 elapsed = qMax<qint64>(
        1, elapsedMillis(calendar, value.start, value.finish));
    return (double(value.durationMillis()) / double(elapsed)) * 10000.0;
}

// Baseline blob timestamps count tenths of a minute from 1983-12-31 (the
// calendar/availability epoch), one day before the cost-rate epoch that
// FieldDecoders' tenths codec uses. With that codec Project read every
// baseline day one day early (verified with TimeScaleData, 2026-09-27).
constexpr qint32 kBaselineEpochShiftTenths = 24 * 60 * 10;

qint32 baselineTimestampTenths(const QDateTime &value)
{
    return FieldDecoders::encodeTimestampTenths(value) + kBaselineEpochShiftTenths;
}

QDateTime decodeBaselineTimestamp(const QByteArray &blob, int offset)
{
    const QDateTime value = FieldDecoders::decodeTimestampTenths(blob, offset);
    return value.isValid() ? value.addDays(-1) : value;
}

} // namespace

namespace Mpp14Timephased {

QList<schedule::TimephasedValue> decodeActualWork(
    const QByteArray &blob, const schedule::Assignment &assignment,
    const schedule::WorkCalendar &calendar, int valueType)
{
    QList<schedule::TimephasedValue> out;
    quint16 count = 0;
    if (blob.size() < 36 || !FieldDecoders::readU16(blob, 0, &count) || count == 0)
        return out;

    double previousWork = 0.0;
    double previousElapsed = 0.0;
    QDateTime start = assignment.start;
    int offset = 36;
    for (quint16 i = 0; i < count && offset + 20 <= blob.size(); ++i, offset += 20) {
        double cumulativeWork = 0.0;
        quint32 encodedElapsed = 0;
        if (!FieldDecoders::readDouble(blob, offset, &cumulativeWork)
            || !FieldDecoders::readU32(blob, offset + 16, &encodedElapsed))
            break;
        const double workMinutes = (cumulativeWork - previousWork) / 1000.0;
        const double cumulativeElapsed = double(encodedElapsed) / 80.0;
        const double periodElapsed = cumulativeElapsed - previousElapsed;
        if (periodElapsed >= 0.0 && start.isValid()) {
            QDateTime finish = calendar.addWork(
                start, qint64(std::llround(periodElapsed * double(kMinuteMs))));
            if (workMinutes > 0.0)
                appendDaily(out, valueType,
                            assignment.uniqueId, start, finish,
                            qint64(std::llround(workMinutes * double(kMinuteMs))),
                            calendar);
            if (periodElapsed > 0.0)
                start = calendar.nextWorkStart(finish);
        }
        previousWork = cumulativeWork;
        previousElapsed = cumulativeElapsed;
    }
    return out;
}

QList<schedule::TimephasedValue> decodeRemainingWork(
    const QByteArray &blob, const schedule::Assignment &assignment,
    const schedule::WorkCalendar &calendar,
    const QList<schedule::TimephasedValue> &actualWork)
{
    QList<schedule::TimephasedValue> out;
    quint16 count = 0;
    if (blob.size() < 24 || !FieldDecoders::readU16(blob, 0, &count))
        return out;

    QDateTime start = assignment.resume;
    if (!start.isValid()) {
        start = assignment.start;
        for (const schedule::TimephasedValue &value : actualWork)
            if (!start.isValid() || value.finish > start)
                start = value.finish;
        if (!actualWork.isEmpty())
            start = calendar.nextWorkStart(start);
    }

    if (count == 0) {
        double cumulativeWork = 0.0;
        if (FieldDecoders::readDouble(blob, 16, &cumulativeWork)
            && cumulativeWork > 0.0 && start.isValid() && assignment.finish > start)
            appendDaily(out, schedule::TimephasedValue::RemainingWork, assignment.uniqueId,
                        start, assignment.finish,
                        qint64(std::llround(cumulativeWork / 1000.0 * double(kMinuteMs))),
                        calendar);
        return out;
    }

    double previousWork = 0.0;
    double previousElapsed = 0.0;
    int offset = 44;
    for (quint16 i = 0; i < count && offset + 28 <= blob.size(); ++i, offset += 28) {
        double cumulativeWork = 0.0;
        quint32 encodedElapsed = 0;
        if (!FieldDecoders::readDouble(blob, offset, &cumulativeWork)
            || !FieldDecoders::readU32(blob, offset + 24, &encodedElapsed))
            break;
        const double workMinutes = (cumulativeWork - previousWork) / 1000.0;
        const double cumulativeElapsed = double(encodedElapsed) / 80.0;
        const double periodElapsed = cumulativeElapsed - previousElapsed;
        if (periodElapsed >= 0.0 && start.isValid()) {
            QDateTime finish = calendar.addWork(
                start, qint64(std::llround(periodElapsed * double(kMinuteMs))));
            if (workMinutes > 0.0)
                appendDaily(out, schedule::TimephasedValue::RemainingWork,
                            assignment.uniqueId, start, finish,
                            qint64(std::llround(workMinutes * double(kMinuteMs))),
                            calendar);
            if (periodElapsed > 0.0)
                start = calendar.nextWorkStart(finish);
        }
        previousWork = cumulativeWork;
        previousElapsed = cumulativeElapsed;
    }
    return out;
}

QByteArray encodeActualWork(const QList<schedule::TimephasedValue> &all,
                            const schedule::WorkCalendar &calendar,
                            QDateTime anchor, int valueType)
{
    const QList<schedule::TimephasedValue> values =
        sortedValues(all, valueType);
    if (values.isEmpty())
        return {};

    if (!anchor.isValid() || anchor > values.first().start)
        anchor = values.first().start;
    struct Record {
        qint64 cumulativeWork = 0;
        qint64 cumulativeElapsed = 0;
        double rate = 0.0;
    };
    QList<Record> records;
    qint64 cumulativeWork = 0;
    qint64 cumulativeElapsed = 0;
    QDateTime cursor = anchor;
    for (const auto &value : values) {
        // A gap record represents working time with zero work. Overnight,
        // weekends, and pre-shift time consume no native elapsed-work minutes.
        const qint64 gap = cursor < value.start
            ? qMax<qint64>(0, calendar.workBetween(cursor, value.start)) : 0;
        if (gap > 0) {
            cumulativeElapsed += gap;
            records.append({cumulativeWork, cumulativeElapsed, 0.0});
        }
        cumulativeWork += value.durationMillis();
        cumulativeElapsed += elapsedMillis(calendar, value.start, value.finish);
        records.append({cumulativeWork, cumulativeElapsed,
                        actualRate(value, calendar)});
        cursor = value.finish;
    }
    if (records.size() > std::numeric_limits<quint16>::max())
        return {};

    const double headerRate = actualRate(values.first(), calendar);
    QByteArray bytes;
    appendU16(bytes, quint16(records.size()));
    appendU16(bytes, 0x18);
    appendU32(bytes, 0x24);
    appendDouble(bytes, headerRate);
    appendDouble(bytes, double(cumulativeWork) / 60.0);
    appendU32(bytes, quint32(std::llround(
        double(cumulativeElapsed) / double(kMinuteMs) * 80.0)));
    bytes.append(8, '\0');

    for (const Record &record : records) {
        appendDouble(bytes, double(record.cumulativeWork) / 60.0);
        appendDouble(bytes, record.rate);
        appendU32(bytes, quint32(std::llround(
            double(record.cumulativeElapsed) / double(kMinuteMs) * 80.0)));
    }
    return bytes;
}

QByteArray encodeRemainingWork(const QList<schedule::TimephasedValue> &all,
                               const schedule::WorkCalendar &calendar,
                               QDateTime anchor, int workContour)
{
    const QList<schedule::TimephasedValue> values =
        sortedValues(all, schedule::TimephasedValue::RemainingWork);
    if (values.isEmpty())
        return {};

    if (!anchor.isValid() || anchor > values.first().start)
        anchor = values.first().start;
    struct Record {
        qint64 cumulativeWork = 0;
        qint64 cumulativeElapsed = 0;
        double rate = 0.0;
    };
    QList<Record> records;
    qint64 cumulativeWork = 0;
    qint64 cumulativeElapsed = 0;
    QDateTime cursor = anchor;
    for (const auto &value : values) {
        const qint64 gap = cursor < value.start
            ? qMax<qint64>(0, calendar.workBetween(cursor, value.start)) : 0;
        if (gap > 0) {
            cumulativeElapsed += gap;
            records.append({cumulativeWork, cumulativeElapsed, 0.0});
        }
        cumulativeWork += value.durationMillis();
        cumulativeElapsed += elapsedMillis(calendar, value.start, value.finish);
        records.append({cumulativeWork, cumulativeElapsed,
                        plannedRate(value, calendar)});
        cursor = value.finish;
    }
    if (records.size() > std::numeric_limits<quint16>::max())
        return {};

    const double headerRate = plannedRate(values.first(), calendar);
    QByteArray bytes;
    appendU16(bytes, quint16(records.size()));
    appendU16(bytes, 0x20);
    appendU32(bytes, 0x2c);
    appendDouble(bytes, headerRate);
    appendDouble(bytes, double(cumulativeWork) / 60.0);
    appendU32(bytes, quint32(std::llround(
        double(cumulativeElapsed) / double(kMinuteMs) * 80.0)));
    appendU32(bytes, quint32(qBound(0, workContour, 8)));
    appendU16(bytes, 1);
    bytes.append(10, '\0');

    for (const Record &record : records) {
        appendDouble(bytes, double(record.cumulativeWork) / 60.0);
        appendDouble(bytes, record.rate);
        appendDouble(bytes, 0.0);
        appendU32(bytes, quint32(std::llround(
            double(record.cumulativeElapsed) / double(kMinuteMs) * 80.0)));
    }
    return bytes;
}

int decodeWorkContour(const QByteArray &remainingWorkBlob)
{
    quint16 contour = 0;
    if (remainingWorkBlob.size() >= 30
        && FieldDecoders::readU16(remainingWorkBlob, 28, &contour))
        return qBound(0, int(contour), 8);
    return 0;
}

QList<schedule::TimephasedValue> decodeBaselineWork(
    const QByteArray &blob, int assignmentUid, int baselineNumber)
{
    QList<schedule::TimephasedValue> out;
    quint16 blockCount = 0;
    if (blob.size() < 48 || !FieldDecoders::readU16(blob, 0, &blockCount)
        || blockCount < 3)
        return out;
    QDateTime start = decodeBaselineTimestamp(blob, 44);
    double previous = 0.0;
    int offset = 48;
    for (int i = 0; i < int(blockCount) - 2 && offset + 20 <= blob.size(); ++i, offset += 20) {
        double cumulative = 0.0;
        if (!FieldDecoders::readDouble(blob, offset, &cumulative))
            break;
        const QDateTime finish = decodeBaselineTimestamp(blob, offset + 16);
        const qint64 work = qint64(std::llround((cumulative - previous) / 1000.0 * kMinuteMs));
        if (start.isValid() && finish > start && work > 0) {
            schedule::TimephasedValue value;
            value.type = schedule::TimephasedValue::BaselineWork;
            value.uniqueId = assignmentUid;
            value.baselineNumber = baselineNumber;
            value.start = start; value.finish = finish; value.unit = 1;
            value.value = isoDuration(work);
            out.append(value);
        }
        start = finish;
        previous = cumulative;
    }
    return out;
}

QList<schedule::TimephasedValue> decodeBaselineCost(
    const QByteArray &blob, int assignmentUid, int baselineNumber)
{
    QList<schedule::TimephasedValue> out;
    quint16 blockCount = 0;
    if (blob.size() < 36 || !FieldDecoders::readU16(blob, 0, &blockCount)
        || blockCount < 3)
        return out;
    QDateTime start = decodeBaselineTimestamp(blob, 32);
    double previous = 0.0;
    int offset = 36;
    for (int i = 0; i < int(blockCount) - 2 && offset + 20 <= blob.size(); ++i, offset += 20) {
        double cumulative = 0.0;
        if (!FieldDecoders::readDouble(blob, offset + 8, &cumulative))
            break;
        const QDateTime finish = decodeBaselineTimestamp(blob, offset + 16);
        const double cost = (cumulative - previous) / 100.0;
        if (start.isValid() && finish > start && cost != 0.0) {
            schedule::TimephasedValue value;
            value.type = schedule::TimephasedValue::BaselineCost;
            value.uniqueId = assignmentUid;
            value.baselineNumber = baselineNumber;
            value.start = start; value.finish = finish; value.unit = 1;
            value.value = QString::number(cost, 'g', 15);
            out.append(value);
        }
        start = finish;
        previous = cumulative;
    }
    return out;
}

// Baseline timephased blobs, byte-for-byte as Project 2016 writes them
// (assignment keys 52/53, task keys 173/174; RE'd 2026-09-27 from a file
// Project baselined itself). Both are a run of 20-byte records headed by a
// start record and closed by an end record; `count` covers start, data and end.
// Work blob: 8-byte header [count][0x10][0x1c][0], a summary record, then
//   [cumulative work f64 (minutes*1000)][segment work u32 (tenths of minutes)]
//   [flag u32: 1 = start/gap/end][finish timestamp u32].
// Cost blob: 16-byte header [count][4][0x10], then
//   [flag u16][marker u32][0 u16][cumulative cost f64 (cents)][finish u32].
// Project leaves uninitialised memory in the cost marker except for the end
// record's 0x20; it reads neither. Without these blobs Project has no baseline
// spread, so BCWS/BCWP (and with them SV/CPI) are 0.
namespace {
const quint8 kBaselineStartSentinel[8] = { 0x8d, 0xed, 0xb5, 0xa0, 0xf7, 0xc6, 0xb0, 0xbe };
constexpr quint32 kBaselineEndTimestamp = 0x340a75f7u;
} // namespace

QByteArray encodeBaselineWork(const QList<schedule::TimephasedValue> &all,
                              int baselineNumber, const schedule::WorkCalendar *calendar)
{
    const auto values = sortedBaselineValues(
        all, schedule::TimephasedValue::BaselineWork, baselineNumber);
    if (values.isEmpty())
        return {};
    // Each record's span is the WORKING time the segment covers (Project
    // spreads the work evenly over it), not its work: a 7 h day at 8 h
    // working time is 420 work over 480. Without a calendar, assume 100%
    // units.
    struct Record { qint64 cumulative = 0; qint64 span = 0; bool gap = false; QDateTime finish; };
    QList<Record> records;
    qint64 cumulative = 0;
    qint64 totalSpan = 0;
    QDateTime cursor = values.first().start;
    for (const auto &value : values) {
        if (cursor < value.start)
            records.append({cumulative, 0, true, value.start});
        qint64 span = calendar ? calendar->workBetween(value.start, value.finish) : 0;
        if (span <= 0)
            span = value.durationMillis();
        cumulative += value.durationMillis();
        totalSpan += span;
        records.append({cumulative, span, false, value.finish});
        cursor = qMax(cursor, value.finish);
    }
    if (records.size() > std::numeric_limits<quint16>::max() - 2)
        return {};
    const auto workUnits = [](qint64 millis) { return double(millis) / 60.0; };
    const auto tenths = [](qint64 millis) { return quint32(qMax<qint64>(0, millis / 6000)); };

    QByteArray bytes(8, '\0');
    pokeU16(bytes, 0, quint16(records.size() + 2));
    pokeU16(bytes, 2, 0x10);
    pokeU16(bytes, 4, 0x1c);
    QByteArray summary(20, '\0');
    pokeDouble(summary, 0, workUnits(cumulative));
    pokeU32(summary, 12, tenths(totalSpan));
    bytes += summary;
    QByteArray startBlock(20, '\0');
    startBlock.replace(0, 8, QByteArrayView(reinterpret_cast<const char *>(kBaselineStartSentinel), 8));
    pokeU32(startBlock, 12, 1);
    pokeU32(startBlock, 16, quint32(baselineTimestampTenths(values.first().start)));
    bytes += startBlock;
    for (const Record &value : records) {
        QByteArray record(20, '\0');
        pokeDouble(record, 0, workUnits(value.cumulative));
        pokeU32(record, 8, tenths(value.span));
        pokeU32(record, 12, value.gap ? 1 : 0);
        pokeU32(record, 16, quint32(baselineTimestampTenths(value.finish)));
        bytes += record;
    }
    QByteArray endBlock(20, '\0');
    pokeDouble(endBlock, 0, workUnits(cumulative));
    pokeU32(endBlock, 12, 1);
    pokeU32(endBlock, 16, kBaselineEndTimestamp);
    bytes += endBlock;
    return bytes;
}

QByteArray encodeBaselineCost(const QList<schedule::TimephasedValue> &all,
                              int baselineNumber)
{
    const auto values = sortedBaselineValues(
        all, schedule::TimephasedValue::BaselineCost, baselineNumber);
    if (values.isEmpty())
        return {};
    struct Record { double cumulative = 0.0; QDateTime finish; };
    QList<Record> records;
    double cumulative = 0.0;
    QDateTime cursor = values.first().start;
    for (const auto &value : values) {
        if (cursor < value.start)
            records.append({cumulative, value.start});
        cumulative += value.value.toDouble() * 100.0;
        records.append({cumulative, value.finish});
        cursor = qMax(cursor, value.finish);
    }
    if (records.size() > std::numeric_limits<quint16>::max() - 2)
        return {};
    QByteArray bytes(16, '\0');
    pokeU16(bytes, 0, quint16(records.size() + 2));
    pokeU16(bytes, 2, 4);
    pokeU16(bytes, 4, 0x10);
    QByteArray startBlock(20, '\0');
    pokeU16(startBlock, 0, 1);
    startBlock.replace(8, 8, QByteArrayView(reinterpret_cast<const char *>(kBaselineStartSentinel), 8));
    pokeU32(startBlock, 16, quint32(baselineTimestampTenths(values.first().start)));
    bytes += startBlock;
    for (const Record &value : records) {
        QByteArray record(20, '\0');
        pokeDouble(record, 8, value.cumulative);
        pokeU32(record, 16, quint32(baselineTimestampTenths(value.finish)));
        bytes += record;
    }
    QByteArray endBlock(20, '\0');
    pokeU16(endBlock, 0, 1);
    pokeU32(endBlock, 2, 0x20);
    pokeDouble(endBlock, 8, cumulative);
    pokeU32(endBlock, 16, kBaselineEndTimestamp);
    bytes += endBlock;
    return bytes;
}

QList<schedule::TimephasedValue> sumBaselineValues(
    const QList<schedule::TimephasedValue> &all, int type, int baselineNumber, int uniqueId)
{
    const auto values = sortedBaselineValues(all, type, baselineNumber);
    QList<QDateTime> cuts;
    for (const auto &value : values) {
        cuts.append(value.start);
        cuts.append(value.finish);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    QList<schedule::TimephasedValue> out;
    for (int i = 0; i + 1 < cuts.size(); ++i) {
        const QDateTime from = cuts.at(i), to = cuts.at(i + 1);
        double total = 0.0;
        bool covered = false;
        for (const auto &value : values) {
            if (value.finish <= from || value.start >= to)
                continue;
            covered = true;
            total += type == schedule::TimephasedValue::BaselineWork
                ? double(value.durationInPeriod(from, to))
                : value.amountInPeriod(from, to);
        }
        if (!covered)
            continue;
        schedule::TimephasedValue sum;
        sum.type = type;
        sum.uniqueId = uniqueId;
        sum.baselineNumber = baselineNumber;
        sum.start = from;
        sum.finish = to;
        sum.unit = 1;
        sum.value = type == schedule::TimephasedValue::BaselineWork
            ? isoDuration(qint64(std::llround(total)))
            : QString::number(total, 'g', 15);
        out.append(sum);
    }
    return out;
}

} // namespace Mpp14Timephased
