// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULEIO_MPP14TIMEPHASED_H
#define SCHEDULEIO_MPP14TIMEPHASED_H

#include "model/assignment.h"
#include "model/workcalendar.h"

#include <QByteArray>
#include <QList>

// Codec for the native MPP14 assignment VarData records:
//   49 = remaining regular work, 50 = actual regular work,
//   51 = actual overtime work.
// The layouts are cumulative-work/cumulative-working-time streams. Their
// structure is also independently documented by MPXJ's TimephasedDataFactory.
namespace Mpp14Timephased {

QList<schedule::TimephasedValue> decodeActualWork(
    const QByteArray &blob, const schedule::Assignment &assignment,
    const schedule::WorkCalendar &calendar,
    int valueType = schedule::TimephasedValue::ActualWork);

QList<schedule::TimephasedValue> decodeRemainingWork(
    const QByteArray &blob, const schedule::Assignment &assignment,
    const schedule::WorkCalendar &calendar,
    const QList<schedule::TimephasedValue> &actualWork);

QByteArray encodeActualWork(const QList<schedule::TimephasedValue> &values,
                            const schedule::WorkCalendar &calendar,
                            QDateTime anchor = QDateTime(),
                            int valueType = schedule::TimephasedValue::ActualWork);
QByteArray encodeRemainingWork(const QList<schedule::TimephasedValue> &values,
                               const schedule::WorkCalendar &calendar,
                               QDateTime anchor = QDateTime(),
                               int workContour = 0);

QList<schedule::TimephasedValue> decodeBaselineWork(
    const QByteArray &blob, int assignmentUid, int baselineNumber);
QList<schedule::TimephasedValue> decodeBaselineCost(
    const QByteArray &blob, int assignmentUid, int baselineNumber);
QByteArray encodeBaselineWork(const QList<schedule::TimephasedValue> &values,
                              int baselineNumber);
QByteArray encodeBaselineCost(const QList<schedule::TimephasedValue> &values,
                              int baselineNumber);

int decodeWorkContour(const QByteArray &remainingWorkBlob);

} // namespace Mpp14Timephased

#endif // SCHEDULEIO_MPP14TIMEPHASED_H
