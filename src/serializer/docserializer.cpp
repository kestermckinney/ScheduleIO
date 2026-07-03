// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "serializer/docserializer.h"
#include "serializer/mpp12serializer.h"
#include "serializer/mpp14serializer.h"
#include "serializer/mpp14writer.h"

#include "codec/bkndvardata.h"
#include "codec/fielddecoders.h"
#include "codec/fieldmap.h"
#include "codec/mppfieldids.h"
#include "codec/propsreader.h"
#include "codec/streamquartet.h"
#include "model/duration.h"
#include "ole/compoundfile.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <numeric>

using FormatVersion = schedule::Project::FormatVersion;

namespace {

// Field type ids for variable-length (string) fields.
constexpr quint16 kFieldName = 1;
constexpr quint16 kFieldInitials = 2;
constexpr quint16 kFieldWbs = 3;
constexpr quint16 kFieldNotes = 4;
// Variable-length blob fields the scaffold uses to round-trip list-valued data.
constexpr quint16 kFieldBaselines = 10;
constexpr quint16 kFieldCustom = 11;
constexpr quint16 kFieldCostRates = 12;
constexpr quint16 kFieldCalData = 13;   // calendar working times + exceptions
constexpr quint16 kFieldTaskExtra = 14; // task actuals + earned-value metrics

// ---- fixed-record packing -------------------------------------------------
// These are the scaffold's own record layouts, NOT the real MPP layouts.
// Replace with the reverse-engineered FixedData layouts once fixtures exist.

void putU32(QByteArray &b, quint32 v)
{ char t[4]; qToLittleEndian<quint32>(v, reinterpret_cast<uchar *>(t)); b.append(t, 4); }
void putI32(QByteArray &b, qint32 v) { putU32(b, static_cast<quint32>(v)); }
void putU16(QByteArray &b, quint16 v)
{ char t[2]; qToLittleEndian<quint16>(v, reinterpret_cast<uchar *>(t)); b.append(t, 2); }
void putU8(QByteArray &b, quint8 v) { b.append(static_cast<char>(v)); }
void putI64(QByteArray &b, qint64 v)
{ char t[8]; qToLittleEndian<quint64>(static_cast<quint64>(v), reinterpret_cast<uchar *>(t)); b.append(t, 8); }
void putDouble(QByteArray &b, double v)
{ quint64 bits; std::memcpy(&bits, &v, 8); char t[8];
  qToLittleEndian<quint64>(bits, reinterpret_cast<uchar *>(t)); b.append(t, 8); }
bool getI64(const QByteArray &b, int off, qint64 *out)
{ quint32 lo = 0, hi = 0;
  if (!FieldDecoders::readU32(b, off, &lo) || !FieldDecoders::readU32(b, off + 4, &hi)) return false;
  *out = static_cast<qint64>((static_cast<quint64>(hi) << 32) | lo); return true; }

// ---- scaffold blob codecs for the list-valued fields --------------------
// These are the scaffold's own self-consistent encodings (used only so the model
// survives read -> write -> read); they are unrelated to the real .mpp layout.

QByteArray packBaselines(const QList<schedule::Baseline> &list)
{
    using namespace FieldDecoders;
    QByteArray b;
    putU32(b, static_cast<quint32>(list.size()));
    for (const schedule::Baseline &x : list) {
        putU32(b, static_cast<quint32>(x.number));
        putDouble(b, x.cost);
        putI64(b, x.workMillis);
        putU32(b, encodeTimestampSeconds(x.start));
        putU32(b, encodeTimestampSeconds(x.finish));
        putI64(b, x.durationMillis);
    }
    return b;
}

QList<schedule::Baseline> unpackBaselines(const QByteArray &b)
{
    using namespace FieldDecoders;
    QList<schedule::Baseline> out;
    quint32 n = 0;
    if (!readU32(b, 0, &n))
        return out;
    int o = 4;
    for (quint32 i = 0; i < n; ++i) {
        schedule::Baseline x;
        quint32 num = 0, s = 0, f = 0;
        double cost = 0.0; qint64 work = 0, dur = 0;
        if (!readU32(b, o, &num)) break;        o += 4;
        if (!readDouble(b, o, &cost)) break;    o += 8;
        if (!getI64(b, o, &work)) break;        o += 8;
        if (!readU32(b, o, &s)) break;          o += 4;
        if (!readU32(b, o, &f)) break;          o += 4;
        if (!getI64(b, o, &dur)) break;         o += 8;
        x.number = static_cast<int>(num);
        x.cost = cost;
        x.workMillis = work;
        x.start = decodeTimestampSeconds(s);
        x.finish = decodeTimestampSeconds(f);
        x.durationMillis = dur;
        out.append(x);
    }
    return out;
}

// Task actuals + earned-value metrics + task-info fields, in a fixed layout
// (dates as second-resolution timestamps, durations/work as i64 ms, the nine
// EVM values as doubles, then manual/effortDriven/taskType/priority/deadline).
QByteArray packTaskExtra(const schedule::Task &t)
{
    using namespace FieldDecoders;
    QByteArray b;
    putU32(b, encodeTimestampSeconds(t.actualStart));
    putU32(b, encodeTimestampSeconds(t.actualFinish));
    putI64(b, t.actualDurationMillis);
    putI64(b, t.actualWorkMillis);
    putDouble(b, t.evm.pv);
    putDouble(b, t.evm.ev);
    putDouble(b, t.evm.ac);
    putDouble(b, t.evm.cv);
    putDouble(b, t.evm.sv);
    putDouble(b, t.evm.cpi);
    putDouble(b, t.evm.spi);
    putDouble(b, t.evm.eac);
    putDouble(b, t.evm.tcpi);
    putU8(b, t.manual ? 1 : 0);
    putU8(b, t.effortDriven ? 1 : 0);
    putU16(b, static_cast<quint16>(t.taskType));
    putU32(b, static_cast<quint32>(t.priority));
    putU32(b, encodeTimestampSeconds(t.deadline));
    return b;
}

void unpackTaskExtra(const QByteArray &b, schedule::Task &t)
{
    using namespace FieldDecoders;
    quint32 s = 0, f = 0;
    if (readU32(b, 0, &s)) t.actualStart = decodeTimestampSeconds(s);
    if (readU32(b, 4, &f)) t.actualFinish = decodeTimestampSeconds(f);
    getI64(b, 8, &t.actualDurationMillis);
    getI64(b, 16, &t.actualWorkMillis);
    int o = 24;
    for (double *p : { &t.evm.pv, &t.evm.ev, &t.evm.ac, &t.evm.cv, &t.evm.sv,
                       &t.evm.cpi, &t.evm.spi, &t.evm.eac, &t.evm.tcpi }) {
        readDouble(b, o, p);
        o += 8;
    }
    if (o + 1 < b.size()) {
        t.manual = b.at(o) != 0;
        t.effortDriven = b.at(o + 1) != 0;
    }
    o += 2;
    quint16 u16v = 0;
    if (readU16(b, o, &u16v)) t.taskType = static_cast<int>(u16v);
    o += 2;
    quint32 u32v = 0;
    if (readU32(b, o, &u32v)) t.priority = static_cast<int>(u32v);
    o += 4;
    if (readU32(b, o, &u32v)) t.deadline = decodeTimestampSeconds(u32v);
}

// Value tags for a custom field's QVariant.
enum CustomTag : quint8 { TagString = 0, TagDouble = 1, TagI64 = 2, TagBool = 3, TagDate = 4 };

QByteArray packCustom(const QList<schedule::CustomField> &list)
{
    using namespace FieldDecoders;
    QByteArray b;
    putU32(b, static_cast<quint32>(list.size()));
    for (const schedule::CustomField &c : list) {
        putI32(b, c.fieldId);
        const QByteArray nm(reinterpret_cast<const char *>(c.name.utf16()), c.name.size() * 2);
        putU32(b, static_cast<quint32>(nm.size()));
        b.append(nm);
        switch (static_cast<QMetaType::Type>(c.value.typeId())) {
        case QMetaType::QString: {
            putU8(b, TagString);
            const QString s = c.value.toString();
            const QByteArray sb(reinterpret_cast<const char *>(s.utf16()), s.size() * 2);
            putU32(b, static_cast<quint32>(sb.size()));
            b.append(sb);
            break;
        }
        case QMetaType::Double:    putU8(b, TagDouble); putDouble(b, c.value.toDouble()); break;
        case QMetaType::LongLong:  putU8(b, TagI64);    putI64(b, c.value.toLongLong()); break;
        case QMetaType::Bool:      putU8(b, TagBool);   putU8(b, c.value.toBool() ? 1 : 0); break;
        case QMetaType::QDateTime: putU8(b, TagDate);   putU32(b, encodeTimestampSeconds(c.value.toDateTime())); break;
        default:                   putU8(b, TagString); putU32(b, 0); break;
        }
    }
    return b;
}

QByteArray packCostRates(const QList<schedule::CostRate> &list)
{
    using namespace FieldDecoders;
    QByteArray b;
    putU32(b, static_cast<quint32>(list.size()));
    for (const schedule::CostRate &c : list) {
        putU32(b, static_cast<quint32>(c.table));
        putU32(b, encodeTimestampSeconds(c.startDate));
        putU32(b, encodeTimestampSeconds(c.endDate));
        putDouble(b, c.standardRate);
        putU32(b, static_cast<quint32>(c.standardRateUnit));
        putDouble(b, c.overtimeRate);
        putU32(b, static_cast<quint32>(c.overtimeRateUnit));
        putDouble(b, c.costPerUse);
    }
    return b;
}

QList<schedule::CostRate> unpackCostRates(const QByteArray &b)
{
    using namespace FieldDecoders;
    QList<schedule::CostRate> out;
    quint32 n = 0;
    if (!readU32(b, 0, &n))
        return out;
    int o = 4;
    for (quint32 i = 0; i < n; ++i) {
        schedule::CostRate c;
        quint32 tbl = 0, s = 0, e = 0, su = 0, ou = 0;
        if (!readU32(b, o, &tbl)) break;        o += 4;
        if (!readU32(b, o, &s)) break;          o += 4;
        if (!readU32(b, o, &e)) break;          o += 4;
        if (!readDouble(b, o, &c.standardRate)) break;  o += 8;
        if (!readU32(b, o, &su)) break;         o += 4;
        if (!readDouble(b, o, &c.overtimeRate)) break;  o += 8;
        if (!readU32(b, o, &ou)) break;         o += 4;
        if (!readDouble(b, o, &c.costPerUse)) break;    o += 8;
        c.table = int(tbl);
        c.startDate = decodeTimestampSeconds(s);
        c.endDate = decodeTimestampSeconds(e);
        c.standardRateUnit = int(su);
        c.overtimeRateUnit = int(ou);
        out.append(c);
    }
    return out;
}

void putUtf16(QByteArray &b, const QString &s)
{ putU32(b, static_cast<quint32>(s.size() * 2)); b.append(reinterpret_cast<const char *>(s.utf16()), s.size() * 2); }

QString getUtf16(const QByteArray &b, int &o)
{
    quint32 len = 0;
    if (!FieldDecoders::readU32(b, o, &len)) { o = b.size(); return QString(); }
    o += 4;
    if (o + int(len) > b.size()) { o = b.size(); return QString(); }
    const QString s = QString::fromUtf16(reinterpret_cast<const char16_t *>(b.constData() + o), int(len) / 2);
    o += int(len);
    return s;
}

void putTimeRanges(QByteArray &b, const QList<schedule::TimeRange> &ranges)
{
    putU32(b, static_cast<quint32>(ranges.size()));
    for (const schedule::TimeRange &r : ranges) {
        putU32(b, static_cast<quint32>(qMax(0, r.start.msecsSinceStartOfDay())));
        putU32(b, static_cast<quint32>(qMax(0, r.end.msecsSinceStartOfDay())));
    }
}

QList<schedule::TimeRange> getTimeRanges(const QByteArray &b, int &o)
{
    QList<schedule::TimeRange> out;
    quint32 n = 0;
    if (!FieldDecoders::readU32(b, o, &n)) { o = b.size(); return out; }
    o += 4;
    for (quint32 i = 0; i < n; ++i) {
        quint32 s = 0, e = 0;
        if (!FieldDecoders::readU32(b, o, &s) || !FieldDecoders::readU32(b, o + 4, &e)) { o = b.size(); break; }
        o += 8;
        out.append({ QTime::fromMSecsSinceStartOfDay(int(s)), QTime::fromMSecsSinceStartOfDay(int(e)) });
    }
    return out;
}

QByteArray packCalData(const QList<QList<schedule::TimeRange>> &hours, const QList<schedule::CalendarException> &exc)
{
    QByteArray b;
    putU32(b, static_cast<quint32>(hours.size()));
    for (const QList<schedule::TimeRange> &day : hours)
        putTimeRanges(b, day);
    putU32(b, static_cast<quint32>(exc.size()));
    for (const schedule::CalendarException &e : exc) {
        putI64(b, e.fromDate.isValid() ? e.fromDate.toJulianDay() : 0);
        putI64(b, e.toDate.isValid() ? e.toDate.toJulianDay() : 0);
        putU8(b, e.working ? 1 : 0);
        putUtf16(b, e.name);
        putTimeRanges(b, e.workingTimes);
    }
    return b;
}

void unpackCalData(const QByteArray &b, QList<QList<schedule::TimeRange>> *hours, QList<schedule::CalendarException> *exc)
{
    using namespace FieldDecoders;
    int o = 0;
    quint32 dayCount = 0;
    if (!readU32(b, o, &dayCount)) return;
    o += 4;
    for (quint32 i = 0; i < dayCount; ++i)
        hours->append(getTimeRanges(b, o));
    quint32 exCount = 0;
    if (!readU32(b, o, &exCount)) return;
    o += 4;
    for (quint32 i = 0; i < exCount; ++i) {
        schedule::CalendarException e;
        qint64 fj = 0, tj = 0;
        if (!getI64(b, o, &fj)) break; o += 8;
        if (!getI64(b, o, &tj)) break; o += 8;
        if (o >= b.size()) break;
        e.working = b.at(o) != 0; o += 1;
        e.fromDate = fj > 0 ? QDate::fromJulianDay(fj) : QDate();
        e.toDate = tj > 0 ? QDate::fromJulianDay(tj) : QDate();
        e.name = getUtf16(b, o);
        e.workingTimes = getTimeRanges(b, o);
        exc->append(e);
    }
}

QList<schedule::CustomField> unpackCustom(const QByteArray &b)
{
    using namespace FieldDecoders;
    QList<schedule::CustomField> out;
    quint32 n = 0;
    if (!readU32(b, 0, &n))
        return out;
    int o = 4;
    for (quint32 i = 0; i < n; ++i) {
        schedule::CustomField c;
        qint32 fid = 0; quint32 nlen = 0;
        if (!readI32(b, o, &fid)) break;        o += 4;
        if (!readU32(b, o, &nlen)) break;       o += 4;
        if (o + static_cast<int>(nlen) > b.size()) break;
        c.fieldId = fid;
        c.name = QString::fromUtf16(reinterpret_cast<const char16_t *>(b.constData() + o), nlen / 2);
        o += static_cast<int>(nlen);
        if (o >= b.size()) break;
        const quint8 tag = static_cast<quint8>(b.at(o)); o += 1;
        switch (tag) {
        case TagString: {
            quint32 slen = 0;
            if (!readU32(b, o, &slen)) { o = b.size(); break; }
            o += 4;
            if (o + static_cast<int>(slen) > b.size()) { o = b.size(); break; }
            c.value = QString::fromUtf16(reinterpret_cast<const char16_t *>(b.constData() + o), slen / 2);
            o += static_cast<int>(slen);
            break;
        }
        case TagDouble: { double v = 0.0; readDouble(b, o, &v); c.value = v; o += 8; break; }
        case TagI64:    { qint64 v = 0; getI64(b, o, &v); c.value = QVariant::fromValue<qint64>(v); o += 8; break; }
        case TagBool:   { c.value = (o < b.size() && b.at(o) != 0); o += 1; break; }
        case TagDate:   { quint32 s = 0; readU32(b, o, &s); c.value = decodeTimestampSeconds(s); o += 4; break; }
        default: o = b.size(); break;
        }
        out.append(c);
    }
    return out;
}

QByteArray packTask(const schedule::Task &t)
{
    using namespace FieldDecoders;
    QByteArray r;
    putU32(r, static_cast<quint32>(t.uniqueId));
    putU32(r, static_cast<quint32>(t.id));
    putU32(r, static_cast<quint32>(t.outlineLevel));
    putU32(r, encodeTimestampSeconds(t.start));
    putU32(r, encodeTimestampSeconds(t.finish));
    putI32(r, encodeDurationTenthMinutes(t.durationMillis));
    putU16(r, encodePercent(t.percentComplete));
    putU8(r, t.milestone ? 1 : 0);
    putU8(r, t.summary ? 1 : 0);
    putU16(r, static_cast<quint16>(t.constraintType));
    putU32(r, encodeTimestampSeconds(t.constraintDate));
    putDouble(r, t.cost);
    putDouble(r, t.fixedCost);
    putDouble(r, t.actualCost);
    putDouble(r, t.remainingCost);
    putDouble(r, t.costVariance);
    putU16(r, static_cast<quint16>(t.durationFormat));
    return r;   // 76 bytes
}
constexpr int kTaskRecordSize = 76;

schedule::Task unpackTask(const QByteArray &r)
{
    using namespace FieldDecoders;
    schedule::Task t;
    quint32 u = 0; qint32 i = 0; quint16 s = 0;
    readU32(r, 0, &u);  t.uniqueId = static_cast<int>(u);
    readU32(r, 4, &u);  t.id = static_cast<int>(u);
    readU32(r, 8, &u);  t.outlineLevel = static_cast<int>(u);
    readU32(r, 12, &u); t.start = decodeTimestampSeconds(u);
    readU32(r, 16, &u); t.finish = decodeTimestampSeconds(u);
    readI32(r, 20, &i); t.durationMillis = decodeDurationTenthMinutes(i);
    readU16(r, 24, &s); t.percentComplete = decodePercent(s);
    t.milestone = r.size() > 26 && r.at(26) != 0;
    t.summary   = r.size() > 27 && r.at(27) != 0;
    quint16 ct = 0;
    if (readU16(r, 28, &ct)) t.constraintType = static_cast<int>(ct);
    if (readU32(r, 30, &u))  t.constraintDate = decodeTimestampSeconds(u);
    readDouble(r, 34, &t.cost);
    readDouble(r, 42, &t.fixedCost);
    readDouble(r, 50, &t.actualCost);
    readDouble(r, 58, &t.remainingCost);
    readDouble(r, 66, &t.costVariance);
    quint16 df = 0;
    if (readU16(r, 74, &df))
        t.durationFormat = static_cast<int>(df);
    return t;
}

QByteArray packResource(const schedule::Resource &res)
{
    QByteArray r;
    putU32(r, static_cast<quint32>(res.uniqueId));
    putU32(r, static_cast<quint32>(res.id));
    putU32(r, static_cast<quint32>(qRound(res.maxUnits * 100000.0)));
    putDouble(r, res.cost);
    putDouble(r, res.actualCost);
    putDouble(r, res.remainingCost);
    putDouble(r, res.costVariance);
    return r;   // 44 bytes
}
constexpr int kResourceRecordSize = 44;

schedule::Resource unpackResource(const QByteArray &r)
{
    schedule::Resource res;
    quint32 u = 0;
    FieldDecoders::readU32(r, 0, &u);  res.uniqueId = static_cast<int>(u);
    FieldDecoders::readU32(r, 4, &u);  res.id = static_cast<int>(u);
    FieldDecoders::readU32(r, 8, &u);  res.maxUnits = static_cast<double>(u) / 100000.0;
    FieldDecoders::readDouble(r, 12, &res.cost);
    FieldDecoders::readDouble(r, 20, &res.actualCost);
    FieldDecoders::readDouble(r, 28, &res.remainingCost);
    FieldDecoders::readDouble(r, 36, &res.costVariance);
    return res;
}

QByteArray packAssignment(const schedule::Assignment &a)
{
    QByteArray r;
    putU32(r, static_cast<quint32>(a.uniqueId));
    putU32(r, static_cast<quint32>(a.taskUniqueId));
    putU32(r, static_cast<quint32>(a.resourceUniqueId));
    putU32(r, static_cast<quint32>(qRound(a.units * 100000.0)));
    putI32(r, FieldDecoders::encodeDurationTenthMinutes(a.workMillis));
    putDouble(r, a.cost);
    putDouble(r, a.actualCost);
    putDouble(r, a.remainingCost);
    putDouble(r, a.costVariance);
    return r;   // 52 bytes
}
constexpr int kAssignmentRecordSize = 52;

schedule::Assignment unpackAssignment(const QByteArray &r)
{
    schedule::Assignment a;
    quint32 u = 0; qint32 i = 0;
    FieldDecoders::readU32(r, 0, &u);  a.uniqueId = static_cast<int>(u);
    FieldDecoders::readU32(r, 4, &u);  a.taskUniqueId = static_cast<int>(u);
    FieldDecoders::readU32(r, 8, &u);  a.resourceUniqueId = static_cast<int>(u);
    FieldDecoders::readU32(r, 12, &u); a.units = static_cast<double>(u) / 100000.0;
    FieldDecoders::readI32(r, 16, &i); a.workMillis = FieldDecoders::decodeDurationTenthMinutes(i);
    FieldDecoders::readDouble(r, 20, &a.cost);
    FieldDecoders::readDouble(r, 28, &a.actualCost);
    FieldDecoders::readDouble(r, 36, &a.remainingCost);
    FieldDecoders::readDouble(r, 44, &a.costVariance);
    return a;
}

QByteArray packRelation(const schedule::Relation &r)
{
    QByteArray b;
    putU32(b, static_cast<quint32>(r.uniqueId));
    putU32(b, static_cast<quint32>(r.predecessorTaskUid));
    putU32(b, static_cast<quint32>(r.successorTaskUid));
    putU32(b, static_cast<quint32>(r.type));
    putI32(b, FieldDecoders::encodeDurationTenthMinutes(r.lagMillis));
    putU16(b, static_cast<quint16>(r.lagFormat));
    return b;   // 22 bytes
}
constexpr int kRelationRecordSize = 22;

schedule::Relation unpackRelation(const QByteArray &b)
{
    schedule::Relation r;
    quint32 u = 0; qint32 i = 0;
    FieldDecoders::readU32(b, 0, &u);  r.uniqueId = static_cast<int>(u);
    FieldDecoders::readU32(b, 4, &u);  r.predecessorTaskUid = static_cast<int>(u);
    FieldDecoders::readU32(b, 8, &u);  r.successorTaskUid = static_cast<int>(u);
    FieldDecoders::readU32(b, 12, &u); r.type = static_cast<int>(u);
    FieldDecoders::readI32(b, 16, &i); r.lagMillis = FieldDecoders::decodeDurationTenthMinutes(i);
    quint16 lf = 0;
    if (FieldDecoders::readU16(b, 20, &lf))
        r.lagFormat = static_cast<int>(lf);
    return r;
}

QByteArray packCalendar(const schedule::Calendar &c)
{
    QByteArray b;
    putU32(b, static_cast<quint32>(c.uniqueId));
    putI32(b, c.baseCalendarUniqueId);
    putU8(b, c.workingDayMask);
    return b;   // 9 bytes
}
constexpr int kCalendarRecordSize = 9;

schedule::Calendar unpackCalendar(const QByteArray &b)
{
    schedule::Calendar c;
    quint32 u = 0; qint32 i = 0;
    FieldDecoders::readU32(b, 0, &u);  c.uniqueId = static_cast<int>(u);
    FieldDecoders::readI32(b, 4, &i);  c.baseCalendarUniqueId = i;
    if (b.size() > 8) c.workingDayMask = static_cast<quint8>(b.at(8));
    return c;
}

QByteArray serializeProps(const schedule::Project &p, FormatVersion v)
{
    QByteArray b;
    putU16(b, static_cast<quint16>(static_cast<int>(v)));
    b.append(FieldDecoders::encodeUnicodeString(p.title));
    b.append(FieldDecoders::encodeUnicodeString(p.author));
    putU32(b, FieldDecoders::encodeTimestampSeconds(p.startDate));
    putU32(b, FieldDecoders::encodeTimestampSeconds(p.finishDate));
    putU32(b, FieldDecoders::encodeTimestampSeconds(p.statusDate));
    return b;
}

void deserializeProps(const QByteArray &b, schedule::Project &p)
{
    using namespace FieldDecoders;
    quint16 ver = 0;
    readU16(b, 0, &ver);
    p.formatVersion = static_cast<FormatVersion>(ver);
    int consumed = 0;
    int off = 2;
    p.title = decodeUnicodeString(b, off, &consumed); off += consumed;
    p.author = decodeUnicodeString(b, off, &consumed); off += consumed;
    quint32 u = 0;
    if (readU32(b, off, &u)) { p.startDate = decodeTimestampSeconds(u); off += 4; }
    if (readU32(b, off, &u)) { p.finishDate = decodeTimestampSeconds(u); off += 4; }
    if (readU32(b, off, &u)) { p.statusDate = decodeTimestampSeconds(u); off += 4; }
}

StreamQuartet::Streams readQuartet(const CompoundFile &cf, const QString &entity)
{
    StreamQuartet::Streams s;
    s.fixedMeta = cf.readStream({ QStringLiteral("Project"), entity, QStringLiteral("FixedMeta") });
    s.varMeta   = cf.readStream({ QStringLiteral("Project"), entity, QStringLiteral("VarMeta") });
    s.fixedData = cf.readStream({ QStringLiteral("Project"), entity, QStringLiteral("FixedData") });
    s.var2Data  = cf.readStream({ QStringLiteral("Project"), entity, QStringLiteral("Var2Data") });
    return s;
}

// The main entity data of a real .mpp lives under a storage named "   114"
// (three leading spaces), with sub-storages TBkndTask, TBkndRsc, etc.
const QString kDataStorage = QStringLiteral("   114");

bool isRealMpp(const CompoundFile &cf)
{
    return cf.hasStorage({ kDataStorage, QStringLiteral("TBkndTask") });
}

// Fixed-data offsets of the task fields we read, recovered from the task field
// map. Field indices are MPPTaskField.FIELD_ARRAY positions.
struct TaskFixedOffsets {
    int uniqueId = -1;   // index 86  (block 0)
    int start = -1;      // index 35  (block 0, auto-scheduled)
    int finish = -1;     // index 36  (block 0, auto-scheduled)
    int duration = -1;   // index 29  (block 0, mode-independent)
    int start1 = -1;     // index 1283 (block 1 / Fixed2Data, manual-scheduled)
    int finish1 = -1;    // index 1284 (block 1 / Fixed2Data, manual-scheduled)
    int id = -1;         // index 23  (block 0)
    int percent = -1;    // index 32  (block 0)
    int outline = -1;    // index 249 (block 0)
    int constraintType = -1;   // index 17 (block 0)
    int constraintDate = -1;   // index 18 (block 0)
    // Minimum block size to treat a block as a real (non-null) task record.
    int block0Needed() const { return qMax(uniqueId, duration) + 4; }
};

// Parse the task field map (28-byte entries) per MPXJ createFieldMap, tracking
// the stateful fixed-data block index, and pick out the block-0 fixed offsets.
TaskFixedOffsets parseTaskFixedOffsets(const QByteArray &fm)
{
    using namespace FieldDecoders;
    TaskFixedOffsets off;
    int lastBlockOffset = 0, blockIndex = 0;
    auto setOnce = [](int &slot, int v) { if (slot < 0) slot = v; };

    for (int i = 0; i + 28 <= fm.size(); i += 28) {
        quint32 typeValue = 0;
        quint16 dataBlockOffset = 0, category = 0;
        readU32(fm, i + 12, &typeValue);
        readU16(fm, i + 4, &dataBlockOffset);
        readU16(fm, i + 20, &category);

        const bool fixed = (category != 0x0B && category != 0x64 && dataBlockOffset != 0xFFFF);
        int thisBlock = 0;
        if (fixed) {
            if (dataBlockOffset < lastBlockOffset)
                ++blockIndex;
            lastBlockOffset = dataBlockOffset;
            thisBlock = blockIndex;
        }
        if (!fixed || (typeValue >> 16) != 0x0B40)
            continue;
        const quint16 idx = typeValue & 0xFFFF;
        if (thisBlock == 0) {
            switch (idx) {
            case 86: setOnce(off.uniqueId, dataBlockOffset); break;
            case 35: setOnce(off.start, dataBlockOffset); break;
            case 36: setOnce(off.finish, dataBlockOffset); break;
            case 29: setOnce(off.duration, dataBlockOffset); break;
            case 23: setOnce(off.id, dataBlockOffset); break;
            case 32: setOnce(off.percent, dataBlockOffset); break;
            case 249: setOnce(off.outline, dataBlockOffset); break;
            case 17: setOnce(off.constraintType, dataBlockOffset); break;
            case 18: setOnce(off.constraintDate, dataBlockOffset); break;
            default: break;
            }
        } else if (thisBlock == 1) {
            switch (idx) {
            case 1283: setOnce(off.start1, dataBlockOffset); break;   // "Task Start"
            case 1284: setOnce(off.finish1, dataBlockOffset); break;  // "Task Finish"
            default: break;
            }
        }
    }
    return off;
}

// Split a fixed-data stream into per-item blocks using its FixedMeta (16-byte
// header, fixed-size items whose +4 field is the block's offset into the data).
// The returned vector is indexed by item position (empty entry == no block) so
// that block-0 (FixedData) and block-1 (Fixed2Data) stay aligned. MPXJ FixedData.
QVector<QByteArray> readFixedBlocks(const QByteArray &meta, const QByteArray &data, int itemSize)
{
    using namespace FieldDecoders;
    constexpr int kHeader = 16;
    QVector<QByteArray> blocks;
    if (itemSize <= 0)
        return blocks;
    const int count = (meta.size() - kHeader) / itemSize;
    blocks.reserve(count);
    for (int loop = 0; loop < count; ++loop) {
        quint32 itemOffset = 0, nextOffset = static_cast<quint32>(data.size());
        readU32(meta, kHeader + loop * itemSize + 4, &itemOffset);
        if (loop + 1 < count)
            readU32(meta, kHeader + (loop + 1) * itemSize + 4, &nextOffset);
        if (itemOffset >= static_cast<quint32>(data.size()) || nextOffset <= itemOffset)
            blocks.append(QByteArray());   // keep index alignment
        else
            blocks.append(data.mid(static_cast<int>(itemOffset),
                                   static_cast<int>(nextOffset - itemOffset)));
    }
    return blocks;
}

// Field-map helpers (block0FixedOffsets, entityFieldLocations, fieldMapBytes)
// live in codec/fieldmap.{h,cpp} now, shared with the MPP14 writer.
using FieldMap::block0FixedOffsets;
using FieldMap::fieldMapBytes;

// Var-data keys for the NOTES field per entity (MPXJ MPP*Field NOTES index).
constexpr quint16 kTaskNotesKey = 15;
constexpr quint16 kResourceNotesKey = 20;
constexpr quint16 kAssignmentNotesKey = 71;

// Notes are stored as an 8-bit (Latin1), NUL-terminated RTF string in var data
// (MPXJ reads NOTES via Var2Data.getString, not getUnicodeString). We keep the
// raw RTF source verbatim.
QString readNotesRtf(const BkndVarData &var, quint32 uid, quint16 key)
{
    const QByteArray blob = var.blobFor(uid, key);
    if (blob.isEmpty())
        return QString();
    int len = blob.indexOf('\0');
    if (len < 0)
        len = blob.size();
    return QString::fromLatin1(blob.constData(), len);
}

// Resource cost-rate tables A..E are var-data fields (MPXJ ResourceField.COST_RATE_*).
constexpr quint16 kCostRateVarKey[5] = { 61, 62, 63, 64, 65 };

// Microsoft "until further notice" end date; entries at/after this are open-ended.
// (MPXJ LocalDateTimeHelper.END_DATE_NA = 2049-12-31 23:59.)
inline QDateTime costRateEndNa()
{
    return QDateTime(QDate(2049, 12, 31), QTime(23, 59), Qt::UTC);
}

// Convert a rate stored per-hour into the rate's display unit (MPXJ
// RateHelper.convertFromHours with Microsoft default minutes-per-day/-week). The
// format short is the Microsoft work-time-units value (0xFFFF == hours).
double rateFromHours(double perHour, quint16 fmt)
{
    const int timeUnit = (fmt == 0xFFFF) ? 1 : (static_cast<int>(fmt) - 1);
    switch (timeUnit) {
    case 0:  return perHour / 60.0;                                  // minutes
    case 1:  return perHour;                                         // hours
    case 2:  return perHour * 8.0;                                   // days  (480 min/day)
    case 3:  return perHour * 40.0;                                  // weeks (2400 min/week)
    case 5:  return static_cast<double>(static_cast<qint64>(perHour * 2400.0 * 52.0)) / 60.0; // years
    default: return perHour;   // months/percent/elapsed: left in per-hour terms
    }
}

// Parse one cost-rate-table var blob (MPXJ CostRateTableFactory): 16-byte header,
// then 44-byte entries [stdRate dbl@0][stdFmt u16@8][otRate dbl@16][otFmt u16@24]
// [costPerUse dbl@32 (/100)][endDate tenths@40]. Entries are sorted by end date and
// given start dates from the previous entry's end + 1 minute.
QList<schedule::CostRate> parseCostRateTable(const QByteArray &blob, int table)
{
    using namespace FieldDecoders;
    auto dbl = [](const QByteArray &b, int o) { double v = 0.0; readDouble(b, o, &v); return v; };
    QList<schedule::CostRate> out;
    for (int i = 16; i + 44 <= blob.size(); i += 44) {
        quint16 stdFmt = 0, otFmt = 0;
        readU16(blob, i + 8, &stdFmt);
        readU16(blob, i + 24, &otFmt);
        schedule::CostRate e;
        e.table = table;
        e.standardRate = rateFromHours(dbl(blob, i), stdFmt);
        e.standardRateUnit = (stdFmt == 0xFFFF) ? 2 : int(stdFmt);
        e.overtimeRate = rateFromHours(dbl(blob, i + 16), otFmt);
        e.overtimeRateUnit = (otFmt == 0xFFFF) ? 2 : int(otFmt);
        e.costPerUse = dbl(blob, i + 32) / 100.0;

        QDateTime end = decodeTimestampTenths(blob, i + 40);
        if (end.isValid()) {
            if (end >= costRateEndNa()) {
                end = QDateTime();   // open-ended
            } else {
                // MPP stores the last minute of the range (e.g. 07:59); a value on a
                // 5-minute boundary is actually the next range's start, so step back.
                if (end.time().minute() % 5 == 0)
                    end = end.addSecs(-60);
                if (end.time().second() != 0)
                    continue;   // entries with seconds are noise (MPXJ heuristic)
            }
        }
        e.endDate = end;
        out.append(e);
    }
    // Sort by end date (open-ended/invalid sorts last) and fill in start dates.
    std::sort(out.begin(), out.end(), [](const schedule::CostRate &a, const schedule::CostRate &b) {
        if (a.endDate.isValid() != b.endDate.isValid())
            return a.endDate.isValid();          // valid (earlier) before open-ended
        return a.endDate.isValid() && a.endDate < b.endDate;
    });
    for (int i = 0; i < out.size(); ++i)
        out[i].startDate = (i == 0 || !out[i - 1].endDate.isValid())
                               ? QDateTime() : out[i - 1].endDate.addSecs(60);
    return out;
}

using FieldMap::EntityFieldLoc;
using FieldMap::entityFieldLocations;

// Decode cost scalars, baselines and custom fields for one entity instance, using
// the field-map locations. Fields default to absent (kAbsent) where the entity
// lacks them. Cost/Number are plain 8-byte doubles; Work is a double in tenths of
// a minute; dates are 4-byte MPP timestamps; durations are u32 tenths of a minute.
struct CostOut {
    double *cost = nullptr;
    double *fixedCost = nullptr;
    double *actualCost = nullptr;
    double *remainingCost = nullptr;
    double *costVariance = nullptr;
};

// Optional task-only outputs (actuals + earned value + task-info fields). Left
// all-null for resources/assignments, which have no such fields.
struct TaskExtraOut {
    QDateTime *actualStart = nullptr;
    QDateTime *actualFinish = nullptr;
    qint64 *actualDurationMillis = nullptr;
    qint64 *actualWorkMillis = nullptr;
    schedule::EarnedValue *evm = nullptr;
    int *priority = nullptr;       // PRIORITY (u16, 0..1000)
    int *taskType = nullptr;       // TYPE (u16, 0/1/2)
    QDateTime *deadline = nullptr; // DEADLINE (MPP timestamp)
};

void fillCostBaselineCustom(const QHash<quint16, EntityFieldLoc> &loc,
                            const QByteArray &b0, const QByteArray &b1,
                            const BkndVarData &var, quint32 uid, quint16 highWord,
                            const MppFieldIds::CostFields &costFields,
                            const MppFieldIds::BaselineSet *baselineSets,
                            const QVector<MppFieldIds::CustomFieldDef> &customDefs,
                            const CostOut &costOut,
                            QList<schedule::Baseline> *baselines,
                            QList<schedule::CustomField> *customFields,
                            const TaskExtraOut &extra = TaskExtraOut())
{
    using namespace FieldDecoders;
    using namespace MppFieldIds;

    // Resolve a field index to a byte source + offset (fixed block or var blob).
    auto locate = [&](quint16 idx, QByteArray &src, int &off) -> bool {
        if (idx == kAbsent)
            return false;
        const auto it = loc.constFind(idx);
        if (it == loc.constEnd())
            return false;
        const EntityFieldLoc &L = it.value();
        if (L.block == 0) { src = b0; off = L.offset; return off >= 0 && off < src.size(); }
        if (L.block == 1) { src = b1; off = L.offset; return off >= 0 && off < src.size(); }
        if (L.var) { src = var.blobFor(uid, idx); off = 0; return !src.isEmpty(); }
        return false;
    };
    auto getDouble = [&](quint16 idx, double *outv) -> bool {
        QByteArray src; int off = 0;
        return locate(idx, src, off) && readDouble(src, off, outv);
    };
    auto getDate = [&](quint16 idx) -> QDateTime {
        QByteArray src; int off = 0;
        return locate(idx, src, off) ? decodeMppTimestamp(src, off) : QDateTime();
    };
    auto getDuration = [&](quint16 idx, qint64 *outv) -> bool {
        QByteArray src; int off = 0; quint32 u = 0;
        if (!locate(idx, src, off) || !readU32(src, off, &u))
            return false;
        *outv = decodeDurationTenthMinutes(static_cast<qint32>(u));
        return true;
    };
    auto getWork = [&](quint16 idx, qint64 *outv) -> bool {
        double v = 0.0;
        if (!getDouble(idx, &v))
            return false;
        *outv = decodeWorkDouble(v);
        return true;
    };

    if (costOut.cost)          getDouble(costFields.cost, costOut.cost);
    if (costOut.fixedCost)     getDouble(costFields.fixedCost, costOut.fixedCost);
    if (costOut.actualCost)    getDouble(costFields.actualCost, costOut.actualCost);
    if (costOut.remainingCost) getDouble(costFields.remainingCost, costOut.remainingCost);
    if (costOut.costVariance)  getDouble(costFields.costVariance, costOut.costVariance);

    // Task-only: recorded actuals + stored earned-value metrics.
    if (extra.actualStart) {
        const QDateTime d = getDate(taskActual.start);
        if (d.isValid()) *extra.actualStart = d;
    }
    if (extra.actualFinish) {
        const QDateTime d = getDate(taskActual.finish);
        if (d.isValid()) *extra.actualFinish = d;
    }
    if (extra.actualDurationMillis) getDuration(taskActual.duration, extra.actualDurationMillis);
    if (extra.actualWorkMillis)     getWork(taskActual.work, extra.actualWorkMillis);
    if (extra.evm) {
        getDouble(taskEvm.bcwp, &extra.evm->ev);
        getDouble(taskEvm.bcws, &extra.evm->pv);
        getDouble(taskEvm.acwp, &extra.evm->ac);
        getDouble(taskEvm.cv,   &extra.evm->cv);
        getDouble(taskEvm.sv,   &extra.evm->sv);
        getDouble(taskEvm.cpi,  &extra.evm->cpi);
        getDouble(taskEvm.spi,  &extra.evm->spi);
        getDouble(taskEvm.eac,  &extra.evm->eac);
        getDouble(taskEvm.tcpi, &extra.evm->tcpi);
    }
    auto getU16 = [&](quint16 idx, int *outv) -> bool {
        QByteArray src; int off = 0; quint16 u = 0;
        if (!locate(idx, src, off) || !readU16(src, off, &u))
            return false;
        *outv = static_cast<int>(u);
        return true;
    };
    if (extra.priority) getU16(taskInfo.priority, extra.priority);
    if (extra.taskType) getU16(taskInfo.taskType, extra.taskType);
    if (extra.deadline) {
        const QDateTime d = getDate(taskInfo.deadline);
        if (d.isValid()) *extra.deadline = d;
    }

    if (baselines) {
        for (int n = 0; n < kBaselineCount; ++n) {
            const BaselineSet &bs = baselineSets[n];
            schedule::Baseline b;
            b.number = n;
            bool any = false;
            if (getDouble(bs.cost, &b.cost)) any = true;
            if (getWork(bs.work, &b.workMillis)) any = true;
            const QDateTime s = getDate(bs.start);
            if (s.isValid()) { b.start = s; any = true; }
            const QDateTime f = getDate(bs.finish);
            if (f.isValid()) { b.finish = f; any = true; }
            if (getDuration(bs.duration, &b.durationMillis)) any = true;
            if (any)
                baselines->append(b);
        }
    }

    if (customFields) {
        for (const CustomFieldDef &d : customDefs) {
            QByteArray src; int off = 0;
            if (!locate(d.index, src, off))
                continue;
            QVariant val;
            switch (d.kind) {
            case FieldKind::String: {
                const int n = (src.size() - off) / 2;
                if (n > 0) {
                    QString str = QString::fromUtf16(
                        reinterpret_cast<const char16_t *>(src.constData() + off), n);
                    while (str.endsWith(QChar(u'\0')))
                        str.chop(1);
                    if (!str.isEmpty())
                        val = str;
                }
                break;
            }
            case FieldKind::Number:
            case FieldKind::Currency: {
                double v = 0.0;
                if (readDouble(src, off, &v))
                    val = v;
                break;
            }
            case FieldKind::DateTime: {
                const QDateTime dt = decodeMppTimestamp(src, off);
                if (dt.isValid())
                    val = dt;
                break;
            }
            case FieldKind::Duration: {
                quint32 u = 0;
                if (readU32(src, off, &u))
                    val = QVariant::fromValue<qint64>(decodeDurationTenthMinutes(static_cast<qint32>(u)));
                break;
            }
            case FieldKind::Bool: {
                if (off < src.size())
                    val = (src.at(off) != 0);
                break;
            }
            }
            if (val.isValid())
                customFields->append({ int((quint32(highWord) << 16) | d.index),
                                       QString::fromLatin1(d.name), val });
        }
    }
}

double readDoubleLE(const QByteArray &d, int off)
{
    double v = 0.0;
    FieldDecoders::readDouble(d, off, &v);
    return v;
}

// Resources: names/initials from var data (NAME=1, INITIALS=2), UID/ID/MaxUnits
// from FixedData (resource field map 0x00020015, FixedMeta item size 37).
void readRealResources(const CompoundFile &cf, schedule::Project &out, const PropsReader &props)
{
    const QString rsc = QStringLiteral("TBkndRsc");
    if (!cf.hasStorage({ kDataStorage, rsc }))
        return;

    BkndVarData v;
    v.parse(cf.readStream({ kDataStorage, rsc, QStringLiteral("VarMeta") }),
            cf.readStream({ kDataStorage, rsc, QStringLiteral("Var2Data") }));
    QHash<int, QString> names, initials;
    for (const auto &e : v.stringsForType(1)) names.insert(int(e.uniqueId), e.value);
    for (const auto &e : v.stringsForType(2)) initials.insert(int(e.uniqueId), e.value);

    const QByteArray fm = fieldMapBytes(props, 0x00020015u, 0x03000015u);
    const QHash<quint16, int> off = block0FixedOffsets(fm, 0x0C40);
    const QHash<quint16, EntityFieldLoc> loc = entityFieldLocations(fm, MppFieldIds::kResourceHigh);
    const int uidOff = off.value(27, -1), idOff = off.value(0, -1), maxOff = off.value(4, -1);
    if (uidOff < 0)
        return;

    const QVector<QByteArray> blocks = readFixedBlocks(
        cf.readStream({ kDataStorage, rsc, QStringLiteral("FixedMeta") }),
        cf.readStream({ kDataStorage, rsc, QStringLiteral("FixedData") }), 37);
    QSet<int> seen;
    for (const QByteArray &b : blocks) {
        quint32 uid = 0;
        if (!FieldDecoders::readU32(b, uidOff, &uid) || seen.contains(int(uid)))
            continue;
        if (!names.contains(int(uid)))   // only real, named resources
            continue;
        seen.insert(int(uid));
        schedule::Resource r;
        r.uniqueId = int(uid);
        r.name = names.value(int(uid));
        r.initials = initials.value(int(uid));
        quint32 v32 = 0;
        if (idOff >= 0 && FieldDecoders::readU32(b, idOff, &v32))
            r.id = int(v32);
        if (maxOff >= 0)
            r.maxUnits = readDoubleLE(b, maxOff) / 10000.0;   // stored in ten-thousandths

        CostOut co;
        co.cost = &r.cost; co.actualCost = &r.actualCost;
        co.remainingCost = &r.remainingCost; co.costVariance = &r.costVariance;
        fillCostBaselineCustom(loc, b, QByteArray(), v, uid, MppFieldIds::kResourceHigh,
                               MppFieldIds::resourceCost, MppFieldIds::resourceBaselines,
                               MppFieldIds::resourceCustomFields(), co,
                               &r.baselines, &r.customFields);
        r.notes = readNotesRtf(v, uid, kResourceNotesKey);
        for (int ti = 0; ti < 5; ++ti) {
            const QByteArray rateBlob = v.blobFor(uid, kCostRateVarKey[ti]);
            if (!rateBlob.isEmpty())
                r.costRates.append(parseCostRateTable(rateBlob, ti));
        }
        out.resources.append(r);
    }
}

// Assignments: link task<->resource with units/work, all from FixedData
// (assignment field map 0x00020017, FixedMeta item size 34).
void readRealAssignments(const CompoundFile &cf, schedule::Project &out, const PropsReader &props)
{
    const QString assn = QStringLiteral("TBkndAssn");
    if (!cf.hasStorage({ kDataStorage, assn }))
        return;

    const QByteArray fm = fieldMapBytes(props, 0x00020017u, 0x03000017u);
    const QHash<quint16, int> off = block0FixedOffsets(fm, 0x0F40);
    const QHash<quint16, EntityFieldLoc> loc = entityFieldLocations(fm, MppFieldIds::kAssignmentHigh);
    const int uidOff = off.value(0, -1), taskOff = off.value(1, -1), resOff = off.value(2, -1),
              unitsOff = off.value(7, -1), workOff = off.value(8, -1);
    // Scheduling fields (indices pinned against the fixtures' XML exports):
    // 10 = actual work, 12 = remaining work, 20 = start, 21 = finish, 25 = delay.
    const int actualWorkOff = off.value(10, -1), remainingWorkOff = off.value(12, -1),
              startOff = off.value(20, -1), finishOff = off.value(21, -1),
              delayOff = off.value(25, -1);
    if (taskOff < 0 || resOff < 0)
        return;

    BkndVarData av;
    av.parse(cf.readStream({ kDataStorage, assn, QStringLiteral("VarMeta") }),
             cf.readStream({ kDataStorage, assn, QStringLiteral("Var2Data") }));

    const QByteArray meta = cf.readStream({ kDataStorage, assn, QStringLiteral("FixedMeta") });
    const QVector<QByteArray> blocks = readFixedBlocks(
        meta, cf.readStream({ kDataStorage, assn, QStringLiteral("FixedData") }), 34);
    QSet<int> seen;
    for (int loop = 0; loop < blocks.size(); ++loop) {
        const QByteArray &b = blocks.at(loop);
        // MPXJ ResourceAssignmentFactory: a row is dead when the first byte of
        // its FixedMeta item is non-zero (deleted assignments stay in the file).
        if (16 + loop * 34 < meta.size() && meta.at(16 + loop * 34) != 0)
            continue;
        quint32 taskUid = 0, resUid = 0;
        if (!FieldDecoders::readU32(b, taskOff, &taskUid)
            || !FieldDecoders::readU32(b, resOff, &resUid))
            continue;
        schedule::Assignment a;
        quint32 v32 = 0;
        if (uidOff >= 0 && FieldDecoders::readU32(b, uidOff, &v32))
            a.uniqueId = int(v32);
        if (seen.contains(a.uniqueId))
            continue;
        // MPXJ also requires the unique id to appear in the assignment VarMeta;
        // rows without any var entries are leftovers Project no longer shows.
        if (!av.hasEntriesFor(quint32(a.uniqueId)))
            continue;
        seen.insert(a.uniqueId);
        a.taskUniqueId = int(taskUid);
        a.resourceUniqueId = int(resUid);
        if (unitsOff >= 0)
            a.units = readDoubleLE(b, unitsOff) / 10000.0;   // hundredths of a percent
        // Work doubles are thousandths of a minute (decodeWorkDouble), the same
        // encoding as every other work field. (An earlier read used the tenth-of-
        // a-minute duration decode, inflating work 100x vs the XML oracle.)
        if (workOff >= 0)
            a.workMillis = FieldDecoders::decodeWorkDouble(readDoubleLE(b, workOff));
        if (actualWorkOff >= 0)
            a.actualWorkMillis = FieldDecoders::decodeWorkDouble(readDoubleLE(b, actualWorkOff));
        if (remainingWorkOff >= 0)
            a.remainingWorkMillis
                = FieldDecoders::decodeWorkDouble(readDoubleLE(b, remainingWorkOff));
        if (startOff >= 0)
            a.start = FieldDecoders::decodeMppTimestamp(b, startOff);
        if (finishOff >= 0)
            a.finish = FieldDecoders::decodeMppTimestamp(b, finishOff);
        quint32 delayRaw = 0;
        if (delayOff >= 0 && FieldDecoders::readU32(b, delayOff, &delayRaw))
            a.delayMillis = FieldDecoders::decodeDurationTenthMinutes(
                static_cast<qint32>(delayRaw));

        CostOut co;
        co.cost = &a.cost; co.actualCost = &a.actualCost;
        co.remainingCost = &a.remainingCost; co.costVariance = &a.costVariance;
        fillCostBaselineCustom(loc, b, QByteArray(), av, quint32(a.uniqueId),
                               MppFieldIds::kAssignmentHigh, MppFieldIds::assignmentCost,
                               MppFieldIds::assignmentBaselines,
                               MppFieldIds::assignmentCustomFields(), co,
                               &a.baselines, &a.customFields);
        a.notes = readNotesRtf(av, quint32(a.uniqueId), kAssignmentNotesKey);
        out.assignments.append(a);
    }
}

// Predecessor links live in TBkndCons (MPXJ ConstraintFactory): FixedMeta item
// size 10, FixedData records of 20 bytes. data[0]=relation UID, data[4]=pred
// task UID, data[8]=succ task UID, data[12]=relation type (u16), data[14]=lag
// (i32 tenths of a minute, Project 2013/2016). The meta item's +4 field is the
// record's offset into FixedData; +0 (u16) != 0 marks it dead.
void readRealRelations(const CompoundFile &cf, schedule::Project &out)
{
    const QString cons = QStringLiteral("TBkndCons");
    if (!cf.hasStorage({ kDataStorage, cons }))
        return;
    const QByteArray meta = cf.readStream({ kDataStorage, cons, QStringLiteral("FixedMeta") });
    const QByteArray data = cf.readStream({ kDataStorage, cons, QStringLiteral("FixedData") });
    const int count = (meta.size() - 16) / 10;
    QSet<int> seen;
    for (int loop = 0; loop < count; ++loop) {
        const int metaPos = 16 + loop * 10;
        quint16 dead = 0;
        if (!FieldDecoders::readU16(meta, metaPos, &dead) || dead != 0)
            continue;
        quint32 off = 0;
        if (!FieldDecoders::readU32(meta, metaPos + 4, &off) || off + 14 > quint32(data.size()))
            continue;
        const QByteArray rec = data.mid(static_cast<int>(off), 20);
        quint32 uid = 0, t1 = 0, t2 = 0;
        quint16 type = 0;
        FieldDecoders::readU32(rec, 0, &uid);
        FieldDecoders::readU32(rec, 4, &t1);
        FieldDecoders::readU32(rec, 8, &t2);
        if (t1 == 0 || t2 == 0 || t1 == t2 || seen.contains(int(uid)))
            continue;
        seen.insert(int(uid));
        FieldDecoders::readU16(rec, 12, &type);
        schedule::Relation r;
        r.uniqueId = int(uid);
        r.predecessorTaskUid = int(t1);
        r.successorTaskUid = int(t2);
        r.type = int(type);
        // Lag: a duration value (tenths of a minute) at offset 14 for Project
        // 2013/2016 (MPXJ ConstraintFactory; older files use offset 16). The units
        // word at offset 18 only affects the display unit, not the elapsed time, so
        // milliseconds are value * 6000 like any other MPP duration. readI32 is
        // bounds-safe and leaves the lag at 0 for short records.
        qint32 lagRaw = 0;
        if (FieldDecoders::readI32(rec, 14, &lagRaw))
            r.lagMillis = FieldDecoders::decodeDurationTenthMinutes(lagRaw);
        quint16 lagUnits = 0;
        if (FieldDecoders::readU16(rec, 18, &lagUnits))
            r.lagFormat = schedule::Duration::normalizeUnit(int(lagUnits));
        out.relations.append(r);
    }
}

// Working-day mask (Monday=bit0..Sunday=bit6) from a calendar's CALENDAR_DATA
// (var type 8): 7 days x 60 bytes; u16@(60*i)=default flag, u16@(60*i+2)=period
// count (>0 == working). MPP day index 0=Sunday. When the data is absent the
// calendar uses the default working week (Mon-Fri), i.e. 0x1F.
quint8 calendarWorkingMask(const QByteArray &data)
{
    using namespace FieldDecoders;
    static const int idxToBit[7]  = { 6, 0, 1, 2, 3, 4, 5 };           // Sun,Mon..Sat
    static const bool defWork[7]  = { false, true, true, true, true, true, false };
    quint8 mask = 0;
    for (int i = 0; i < 7; ++i) {
        bool working = defWork[i];
        quint16 flag = 0, periods = 0;
        if (readU16(data, 60 * i, &flag)) {
            if (flag != 1)
                working = readU16(data, 60 * i + 2, &periods) && periods > 0;
        }
        if (working)
            mask |= static_cast<quint8>(1u << idxToBit[i]);
    }
    return mask;
}

// Decode a calendar's working times (7 days, index 0=Monday..6=Sunday) and its
// exceptions from the CALENDAR_DATA blob (var type 8), per MPXJ
// AbstractCalendar(AndException)Factory. Layout: 7 x 60-byte day blocks, then at
// offset 420 the exceptions. Times are tenths-of-a-minute since midnight; an
// exception block is 92 bytes plus a 4-byte-aligned UTF-16 name.
void parseCalendarData(const QByteArray &blob, bool isBaseCalendar,
                       QList<QList<schedule::TimeRange>> *outHours,
                       QList<schedule::CalendarException> *outExceptions)
{
    using namespace FieldDecoders;
    auto mppTime = [](quint16 v) -> QTime {
        qint64 secs = (static_cast<qint64>(v) / 10) * 60;
        if (secs > 86399) secs %= 86400;
        return QTime(0, 0).addSecs(static_cast<int>(secs));
    };
    auto endOf = [](QTime start, quint16 durTenths) -> QTime {
        return start.addMSecs(static_cast<int>(static_cast<qint64>(durTenths) * 6000));
    };
    // blob day index (0=Sunday..6=Saturday) -> our index (0=Monday..6=Sunday).
    static const int idxToDay[7] = { 6, 0, 1, 2, 3, 4, 5 };
    static const bool defWork[7] = { false, true, true, true, true, true, false };
    static const schedule::TimeRange kMorning { QTime(8, 0), QTime(12, 0) };
    static const schedule::TimeRange kAfternoon { QTime(13, 0), QTime(17, 0) };

    outHours->clear();
    for (int d = 0; d < 7; ++d)
        outHours->append(QList<schedule::TimeRange>());

    for (int i = 0; i < 7; ++i) {
        QList<schedule::TimeRange> ranges;
        quint16 flag = 1;
        if (!blob.isEmpty())
            readU16(blob, 60 * i, &flag);
        if (flag == 1) {
            // A "default" day: a base calendar uses the standard working week's
            // hours; a derived (e.g. resource) calendar inherits from its base, so
            // we leave it empty (the consumer resolves via baseCalendarUniqueId).
            if (isBaseCalendar && defWork[i])
                ranges << kMorning << kAfternoon;
        } else {
            quint16 periodCount = 0;
            readU16(blob, 60 * i + 2, &periodCount);
            for (int p = 0; p < periodCount; ++p) {
                quint16 st = 0, dur = 0;
                if (!readU16(blob, 60 * i + 8 + p * 2, &st))
                    break;
                readU16(blob, 60 * i + 20 + p * 4, &dur);
                const QTime start = mppTime(st);
                ranges << schedule::TimeRange{ start, endOf(start, dur) };
            }
        }
        (*outHours)[idxToDay[i]] = ranges;
    }

    if (blob.size() > 420) {
        int offset = 420;
        quint16 exCount = 0;
        readU16(blob, offset, &exCount);
        offset += 4;   // align past the count to the first exception
        for (int k = 0; k < exCount && offset + 92 <= blob.size(); ++k) {
            schedule::CalendarException ex;
            quint16 fromDays = 0, toDays = 0, periodCount = 0;
            readU16(blob, offset, &fromDays);
            readU16(blob, offset + 2, &toDays);
            // Calendar dates count days from 1983-12-31 (same epoch the fixed-data
            // timestamps use), so the decoded date matches Microsoft Project's.
            if (fromDays != 0xFFFF)
                ex.fromDate = QDate(1983, 12, 31).addDays(fromDays);
            if (toDays != 0xFFFF)
                ex.toDate = QDate(1983, 12, 31).addDays(toDays);
            readU16(blob, offset + 14, &periodCount);
            ex.working = periodCount != 0;
            for (int p = 0; p < periodCount; ++p) {
                quint16 st = 0, dur = 0;
                readU16(blob, offset + 20 + p * 2, &st);
                readU16(blob, offset + 32 + p * 4, &dur);
                const QTime start = mppTime(st);
                ex.workingTimes << schedule::TimeRange{ start, endOf(start, dur) };
            }
            quint32 nameLen = 0;
            readU32(blob, offset + 88, &nameLen);
            if (nameLen % 4 != 0)
                nameLen = ((nameLen / 4) + 1) * 4;
            if (nameLen != 0) {
                int p = offset + 92, n = 0;
                quint16 ch = 0;
                while (p + 2 <= blob.size() && readU16(blob, p, &ch) && ch != 0) { ++n; p += 2; }
                ex.name = QString::fromUtf16(
                    reinterpret_cast<const char16_t *>(blob.constData() + offset + 92), n);
            }
            offset += 92 + static_cast<int>(nameLen);
            outExceptions->append(ex);
        }
    }
}

// Like readFixedBlocks, but tolerant of FixedData whose meta offsets are NOT in
// storage order (calendars). Each block runs from its meta offset to the next
// higher offset that actually occurs (MPXJ FixedData semantics), so out-of-order
// records are still extracted. Returned vector is indexed by meta item.
QVector<QByteArray> readVarSizedBlocks(const QByteArray &meta, const QByteArray &data, int metaItemSize)
{
    using namespace FieldDecoders;
    QVector<QByteArray> blocks;
    if (metaItemSize <= 0)
        return blocks;
    const int count = (meta.size() - 16) / metaItemSize;
    QVector<quint32> offs(count);
    for (int i = 0; i < count; ++i)
        readU32(meta, 16 + i * metaItemSize + 4, &offs[i]);
    QVector<quint32> sorted = offs;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    for (int i = 0; i < count; ++i) {
        const quint32 off = offs[i];
        quint32 end = static_cast<quint32>(data.size());
        for (quint32 s : sorted)
            if (s > off) { end = s; break; }
        if (off >= static_cast<quint32>(data.size()) || end <= off)
            blocks.append(QByteArray());
        else
            blocks.append(data.mid(static_cast<int>(off), static_cast<int>(end - off)));
    }
    return blocks;
}

// Calendars (TBkndCal, MPXJ AbstractCalendarFactory / MPP14CalendarFactory):
// FixedMeta item 10, FixedData block 12. For Project 2013/2016: calendarID@8,
// baseCalendarID@0. Name = var-data type 1 (CALENDAR_NAME).
void readRealCalendars(const CompoundFile &cf, schedule::Project &out)
{
    const QString cal = QStringLiteral("TBkndCal");
    if (!cf.hasStorage({ kDataStorage, cal }))
        return;

    BkndVarData v;
    v.parse(cf.readStream({ kDataStorage, cal, QStringLiteral("VarMeta") }),
            cf.readStream({ kDataStorage, cal, QStringLiteral("Var2Data") }));
    QHash<int, QString> names;   // base-calendar names, by calendar id
    for (const auto &e : v.stringsForType(1))
        names.insert(int(e.uniqueId), e.value);

    // Resource calendars take their name from the linked resource.
    QHash<int, QString> resName;
    for (const schedule::Resource &r : out.resources)
        resName.insert(r.uniqueId, r.name);

    const QVector<QByteArray> blocks = readVarSizedBlocks(
        cf.readStream({ kDataStorage, cal, QStringLiteral("FixedMeta") }),
        cf.readStream({ kDataStorage, cal, QStringLiteral("FixedData") }), 10);
    QSet<int> seen;
    for (const QByteArray &b : blocks) {
        quint32 calId = 0, baseId = 0, resId = 0;
        if (!FieldDecoders::readU32(b, 8, &calId) || !FieldDecoders::readU32(b, 0, &baseId)
            || !FieldDecoders::readU32(b, 4, &resId))
            continue;
        if (calId == 0 || seen.contains(int(calId)))
            continue;
        seen.insert(int(calId));
        schedule::Calendar c;
        c.uniqueId = int(calId);
        const bool isBase = (baseId == 0xFFFFFFFFu || baseId == 0 || baseId == calId);
        if (isBase) {
            c.baseCalendarUniqueId = -1;
            c.name = names.value(int(calId));
        } else {
            c.baseCalendarUniqueId = int(baseId);
            c.name = resName.value(int(resId));   // resource calendar -> resource name
        }
        const QByteArray calData = v.blobFor(calId, 8);
        c.workingDayMask = calendarWorkingMask(calData);
        parseCalendarData(calData, isBase, &c.workingTimes, &c.exceptions);
        out.calendars.append(c);
    }
}

// Title/Author from the standard "\005SummaryInformation" OLE property set
// ([MS-OLEPS]): header (section offset at byte 44), section = [size][count] then
// (propId, propOffset) pairs; PIDSI_TITLE=2, PIDSI_AUTHOR=4. String values are
// VT_LPSTR (0x1E, ANSI) or VT_LPWSTR (0x1F, UTF-16), each [u32 len][bytes].
void readSummaryInformation(const CompoundFile &cf, schedule::Project &out)
{
    const QByteArray s = cf.readStream({ QString(QChar(0x05)) + QStringLiteral("SummaryInformation") });
    quint32 sectionOff = 0, propCount = 0;
    if (!FieldDecoders::readU32(s, 44, &sectionOff)
        || !FieldDecoders::readU32(s, static_cast<int>(sectionOff) + 4, &propCount))
        return;

    auto decodeProp = [&](int vpos) -> QString {
        quint32 type = 0, len = 0;
        if (!FieldDecoders::readU32(s, vpos, &type) || !FieldDecoders::readU32(s, vpos + 4, &len))
            return QString();
        const int data = vpos + 8;
        if (len == 0 || data + static_cast<int>(len) > s.size())
            return QString();
        QString out;
        if (type == 0x1E)   // VT_LPSTR (ANSI byte length, includes the NUL)
            out = QString::fromLatin1(s.constData() + data, static_cast<int>(len));
        else if (type == 0x1F)   // VT_LPWSTR (UTF-16 char count, includes the NUL)
            out = QString::fromUtf16(reinterpret_cast<const char16_t *>(s.constData() + data),
                                     static_cast<int>(len));
        while (out.endsWith(QChar(u'\0')))
            out.chop(1);
        return out;
    };

    for (quint32 i = 0; i < propCount; ++i) {
        quint32 pid = 0, poff = 0;
        const int pairPos = static_cast<int>(sectionOff) + 8 + static_cast<int>(i) * 8;
        if (!FieldDecoders::readU32(s, pairPos, &pid) || !FieldDecoders::readU32(s, pairPos + 4, &poff))
            break;
        if (pid == 2)
            out.title = decodeProp(static_cast<int>(sectionOff) + static_cast<int>(poff));
        else if (pid == 4)
            out.author = decodeProp(static_cast<int>(sectionOff) + static_cast<int>(poff));
    }
}

// Reads what we have so far reverse-engineered from real files: task names from
// "   114/TBkndTask" (VarMeta field code 6 -> Var2Data UTF-16 strings). Other
// fields (UID, dates, duration, resources) are the next RE increments.
bool readRealMpp(const CompoundFile &cf, schedule::Project &out, schedule::Project::FormatVersion ver)
{
    out = schedule::Project();
    out.formatVersion = ver;

    const QString task = QStringLiteral("TBkndTask");

    // Names come from var data field 14 (TaskField.NAME); one task per unique id.
    BkndVarData taskVars;
    taskVars.parse(cf.readStream({ kDataStorage, task, QStringLiteral("VarMeta") }),
                   cf.readStream({ kDataStorage, task, QStringLiteral("Var2Data") }));

    QHash<int, int> pos;   // unique id -> index into out.tasks
    for (const BkndVarData::Entry &e : taskVars.stringsForType(BkndVarData::FieldTaskName)) {
        const int uid = int(e.uniqueId);
        if (e.value.isEmpty() || pos.contains(uid))
            continue;
        pos.insert(uid, out.tasks.size());
        schedule::Task t;
        t.uniqueId = uid;
        t.name = e.value;
        out.tasks.append(t);
    }

    // Start / Finish / Duration come from FixedData, located via the task field
    // map in "   114/Props" (TASK_FIELD_MAP=0x00020014, fallback 0x03000014).
    PropsReader props;
    props.parse(cf.readStream({ kDataStorage, QStringLiteral("Props") }));
    QByteArray fmBytes = props.value(0x00020014u);
    if (fmBytes.isEmpty())
        fmBytes = props.value(0x03000014u);
    const TaskFixedOffsets off = parseTaskFixedOffsets(fmBytes);
    const QHash<quint16, EntityFieldLoc> taskLoc =
        entityFieldLocations(fmBytes, MppFieldIds::kTaskHigh);

    if (off.uniqueId >= 0) {
        const int needed = off.block0Needed() + 4;
        const QByteArray fixedMeta = cf.readStream({ kDataStorage, task, QStringLiteral("FixedMeta") });
        const QVector<QByteArray> blocks0 = readFixedBlocks(
            fixedMeta,
            cf.readStream({ kDataStorage, task, QStringLiteral("FixedData") }), 47);

        // Fixed2Data (block 1) holds manual-scheduled dates. Its FixedMeta item
        // size is adaptive (MPXJ tries 92..96); derive it from the item count.
        const QByteArray meta2 = cf.readStream({ kDataStorage, task, QStringLiteral("Fixed2Meta") });
        const QByteArray data2 = cf.readStream({ kDataStorage, task, QStringLiteral("Fixed2Data") });
        const int item2 = (!blocks0.isEmpty() && meta2.size() > 16)
                              ? (meta2.size() - 16) / blocks0.size()
                              : 0;
        const QVector<QByteArray> blocks1 =
            (item2 >= 88 && item2 <= 100) ? readFixedBlocks(meta2, data2, item2)
                                          : QVector<QByteArray>();

        // Effective date: the manual field (block 1) if present, else the
        // auto/computed field (block 0). Matches MS Project's displayed dates.
        auto effectiveDate = [&](int loop, int b1Off, int b0Off, const QByteArray &b0) {
            if (b1Off >= 0 && loop < blocks1.size()) {
                const QDateTime d = FieldDecoders::decodeMppTimestamp(blocks1.at(loop), b1Off);
                if (d.isValid())
                    return d;
            }
            return b0Off >= 0 ? FieldDecoders::decodeMppTimestamp(b0, b0Off) : QDateTime();
        };

        // MPXJ createTaskMap semantics: a FixedMeta item whose flags word has
        // bit 0x02 marks a DELETED row (Project keeps deleted tasks in the
        // file); live rows must also hold >75% of the block-0 record. A task
        // that never gets a live row (name-only ghost) is dropped afterwards.
        int maxFixed0 = 0;
        for (auto it = taskLoc.constBegin(); it != taskLoc.constEnd(); ++it)
            if (it->block == 0)
                maxFixed0 = qMax(maxFixed0, it->offset + 2);

        QSet<int> filled;   // first full LIVE block per unique id wins (matches MPXJ)
        for (int loop = 0; loop < blocks0.size(); ++loop) {
            const QByteArray &b0 = blocks0.at(loop);
            if (b0.size() < needed)
                continue;   // skip null/partial task blocks
            quint32 itemFlags = 0;
            FieldDecoders::readU32(fixedMeta, 16 + loop * 47, &itemFlags);
            if (itemFlags & 0x02u)
                continue;   // deleted row
            if (maxFixed0 > 0 && (b0.size() * 100) / maxFixed0 <= 75)
                continue;   // truncated leftover row (MPXJ 75% heuristic)
            quint32 uid32 = 0;
            FieldDecoders::readU32(b0, off.uniqueId, &uid32);
            const auto it = pos.constFind(int(uid32));
            if (it == pos.constEnd() || filled.contains(int(uid32)))
                continue;
            filled.insert(int(uid32));
            schedule::Task &t = out.tasks[it.value()];
            t.start = effectiveDate(loop, off.start1, off.start, b0);
            t.finish = effectiveDate(loop, off.finish1, off.finish, b0);
            quint32 u32v = 0;
            quint16 u16v = 0;
            if (off.duration >= 0 && FieldDecoders::readU32(b0, off.duration, &u32v))
                t.durationMillis = FieldDecoders::decodeDurationTenthMinutes(static_cast<qint32>(u32v));
            if (off.id >= 0 && FieldDecoders::readU32(b0, off.id, &u32v))
                t.id = static_cast<int>(u32v);
            if (off.percent >= 0 && FieldDecoders::readU16(b0, off.percent, &u16v))
                t.percentComplete = FieldDecoders::decodePercent(u16v);
            if (off.outline >= 0 && FieldDecoders::readU16(b0, off.outline, &u16v))
                t.outlineLevel = static_cast<int>(u16v);
            if (off.constraintType >= 0 && FieldDecoders::readU16(b0, off.constraintType, &u16v))
                t.constraintType = static_cast<int>(u16v);
            if (off.constraintDate >= 0)
                t.constraintDate = FieldDecoders::decodeMppTimestamp(b0, off.constraintDate);

            // MILESTONE is a bit flag in the 47-byte FixedMeta item (MPXJ
            // *_TASK_META_DATA_BIT_FLAGS). Project 2013/2016: int at meta offset
            // 10, mask 0x02. (Project 2010 used offset 8, mask 0x20.)
            quint32 metaFlags = 0;
            if (FieldDecoders::readU32(fixedMeta, 16 + loop * 47 + 10, &metaFlags))
                t.milestone = (metaFlags & 0x02u) != 0;

            // EFFORT_DRIVEN is a bit flag in the same FixedMeta item: int at meta
            // offset 13, mask 0x08 (Project 2013/2016; 2010 used offset 11, 0x10).
            if (FieldDecoders::readU32(fixedMeta, 16 + loop * 47 + 13, &metaFlags))
                t.effortDriven = (metaFlags & 0x08u) != 0;

            // TASK_MODE (manually scheduled) is a bit flag in the per-task
            // Fixed2Meta item: int at offset 8, mask 0x80 (Project 2013/2016;
            // 2010 used mask 0x08). MPXJ *_TASK_META_DATA2_BIT_FLAGS.
            if (item2 > 0
                && FieldDecoders::readU32(meta2, 16 + loop * item2 + 8, &metaFlags))
                t.manual = (metaFlags & 0x80u) != 0;

            // Cost scalars, baselines (0..10) and custom fields.
            const QByteArray b1 = (loop < blocks1.size()) ? blocks1.at(loop) : QByteArray();
            CostOut co;
            co.cost = &t.cost; co.fixedCost = &t.fixedCost; co.actualCost = &t.actualCost;
            co.remainingCost = &t.remainingCost; co.costVariance = &t.costVariance;
            TaskExtraOut ex;
            ex.actualStart = &t.actualStart; ex.actualFinish = &t.actualFinish;
            ex.actualDurationMillis = &t.actualDurationMillis;
            ex.actualWorkMillis = &t.actualWorkMillis; ex.evm = &t.evm;
            ex.priority = &t.priority; ex.taskType = &t.taskType;
            ex.deadline = &t.deadline;
            fillCostBaselineCustom(taskLoc, b0, b1, taskVars, uid32, MppFieldIds::kTaskHigh,
                                   MppFieldIds::taskCost, MppFieldIds::taskBaselines,
                                   MppFieldIds::taskCustomFields(), co,
                                   &t.baselines, &t.customFields, ex);
            t.notes = readNotesRtf(taskVars, uid32, kTaskNotesKey);
        }

        // Drop tasks that never received a live FixedData row: their names
        // survive in Var2Data but the row is deleted (or gone), so MS Project
        // and MPXJ do not show them.
        for (int i = out.tasks.size() - 1; i >= 0; --i)
            if (!filled.contains(out.tasks.at(i).uniqueId))
                out.tasks.removeAt(i);

        // SUMMARY is derived from the outline hierarchy: a task is a summary if
        // a following task (in ID order) sits one outline level deeper, or if it
        // is the project summary row (outline level 0).
        QVector<int> byId(out.tasks.size());
        std::iota(byId.begin(), byId.end(), 0);
        std::sort(byId.begin(), byId.end(),
                  [&](int a, int b) { return out.tasks[a].id < out.tasks[b].id; });
        QVector<int> counters;   // WBS / OutlineNumber counter per outline level
        for (int i = 0; i < byId.size(); ++i) {
            schedule::Task &cur = out.tasks[byId[i]];
            const bool hasChild = (i + 1 < byId.size())
                && out.tasks[byId[i + 1]].outlineLevel > cur.outlineLevel;
            cur.summary = hasChild || cur.outlineLevel == 0;

            // WBS == OutlineNumber: dotted path of per-level positions (the
            // project-summary row at level 0 is "0").
            const int level = cur.outlineLevel;
            if (level <= 0) {
                cur.wbs = QStringLiteral("0");
            } else {
                if (counters.size() < level)
                    counters.resize(level);   // new deeper levels start at 0
                else
                    counters.resize(level);   // truncate back to this level
                ++counters[level - 1];
                QStringList parts;
                for (int c : counters)
                    parts << QString::number(c);
                cur.wbs = parts.join(QLatin1Char('.'));
            }
        }
    }

    readRealResources(cf, out, props);
    readRealAssignments(cf, out, props);

    // Resource cost is frequently not stored on the resource itself (Microsoft
    // Project computes it from cost-rate tables, which we do not decode yet). Where
    // a resource has no stored cost, roll it up from its assignment costs so the
    // model matches the resource cost MS Project displays/exports.
    if (!out.assignments.isEmpty() && !out.resources.isEmpty()) {
        QHash<int, double> costByRes, actualByRes, remainByRes;
        for (const schedule::Assignment &a : out.assignments) {
            costByRes[a.resourceUniqueId]   += a.cost;
            actualByRes[a.resourceUniqueId] += a.actualCost;
            remainByRes[a.resourceUniqueId] += a.remainingCost;
        }
        for (schedule::Resource &r : out.resources) {
            if (r.cost == 0.0)          r.cost = costByRes.value(r.uniqueId, 0.0);
            if (r.actualCost == 0.0)    r.actualCost = actualByRes.value(r.uniqueId, 0.0);
            if (r.remainingCost == 0.0) r.remainingCost = remainByRes.value(r.uniqueId, 0.0);
        }
    }

    readRealRelations(cf, out);
    readRealCalendars(cf, out);
    readSummaryInformation(cf, out);

    // Project start/finish dates (PropsKey PROJECT_START_DATE=0x02400002,
    // PROJECT_FINISH_DATE=0x02400003) live in "   114/Props" as MPP timestamps.
    auto projDate = [](const PropsReader &pr, quint32 key) -> QDateTime {
        const QByteArray v = pr.value(key);
        return v.size() >= 4 ? FieldDecoders::decodeMppTimestamp(v, 0) : QDateTime();
    };
    out.startDate = projDate(props, 0x02400002u);
    out.finishDate = projDate(props, 0x02400003u);
    out.statusDate = projDate(props, 0x02400045u);   // PropsKey STATUS_DATE = 37748805
    return true;
}

void writeQuartet(CompoundFile &cf, const QString &entity, const StreamQuartet::Streams &s)
{
    cf.addStream({ QStringLiteral("Project"), entity, QStringLiteral("FixedMeta") }, s.fixedMeta);
    cf.addStream({ QStringLiteral("Project"), entity, QStringLiteral("VarMeta") },   s.varMeta);
    cf.addStream({ QStringLiteral("Project"), entity, QStringLiteral("FixedData") }, s.fixedData);
    cf.addStream({ QStringLiteral("Project"), entity, QStringLiteral("Var2Data") },  s.var2Data);
}

} // namespace

// ---------------------------------------------------------------------------

FormatVersion DocSerializer::detectVersion(const CompoundFile &cf)
{
    if (!cf.isValid())
        return FormatVersion::Unknown;

    // Real MS Project files carry a top-level "Props<NN>" stream whose suffix is
    // the format version (confirmed against fixtures: "Props14" -> MPP.14).
    if (cf.hasStream({ QStringLiteral("Props14") }))
        return FormatVersion::Mpp14;
    if (cf.hasStream({ QStringLiteral("Props12") }))
        return FormatVersion::Mpp12;

    // The scaffold's own written files store the version in Project/Props.
    const QByteArray props = cf.readStream({ QStringLiteral("Project"), QStringLiteral("Props") });
    quint16 ver = 0;
    if (FieldDecoders::readU16(props, 0, &ver)) {
        if (ver == 12) return FormatVersion::Mpp12;
        if (ver == 14) return FormatVersion::Mpp14;
    }
    return FormatVersion::Unknown;
}

std::unique_ptr<DocSerializer> DocSerializer::create(FormatVersion v)
{
    switch (v) {
    case FormatVersion::Mpp12: return std::make_unique<Mpp12Serializer>();
    case FormatVersion::Mpp14: return std::make_unique<Mpp14Serializer>();
    default: return nullptr;
    }
}

bool DocSerializer::read(const CompoundFile &cf, schedule::Project &out, QString *error) const
{
    if (!cf.isValid()) {
        if (error) *error = QStringLiteral("invalid compound file");
        return false;
    }

    // Real Microsoft Project files: read what we have reverse-engineered so far.
    if (isRealMpp(cf))
        return readRealMpp(cf, out, version());

    // Otherwise fall back to the scaffold's own self-consistent format.
    if (!cf.hasStorage({ QStringLiteral("Project") })) {
        if (error) *error = QStringLiteral("missing 'Project' storage");
        return false;
    }

    out = schedule::Project();
    deserializeProps(cf.readStream({ QStringLiteral("Project"), QStringLiteral("Props") }), out);

    QString qerr;
    const StreamQuartet tasks = StreamQuartet::decode(readQuartet(cf, QStringLiteral("Task")), &qerr);
    for (int i = 0; i < tasks.fixedRecords.size(); ++i) {
        schedule::Task t = unpackTask(tasks.fixedRecords.at(i));
        for (const auto &ve : tasks.varEntries) {
            if (static_cast<int>(ve.itemIndex) != i)
                continue;
            if (ve.fieldType == kFieldBaselines) { t.baselines = unpackBaselines(ve.data); continue; }
            if (ve.fieldType == kFieldCustom)    { t.customFields = unpackCustom(ve.data); continue; }
            if (ve.fieldType == kFieldTaskExtra) { unpackTaskExtra(ve.data, t); continue; }
            const QString s = QString::fromUtf16(
                reinterpret_cast<const char16_t *>(ve.data.constData()), ve.data.size() / 2);
            if (ve.fieldType == kFieldName) t.name = s;
            else if (ve.fieldType == kFieldWbs) t.wbs = s;
            else if (ve.fieldType == kFieldNotes) t.notes = s;
        }
        out.tasks.append(t);
    }

    const StreamQuartet res = StreamQuartet::decode(readQuartet(cf, QStringLiteral("Resource")), &qerr);
    for (int i = 0; i < res.fixedRecords.size(); ++i) {
        schedule::Resource r = unpackResource(res.fixedRecords.at(i));
        for (const auto &ve : res.varEntries) {
            if (static_cast<int>(ve.itemIndex) != i)
                continue;
            if (ve.fieldType == kFieldBaselines) { r.baselines = unpackBaselines(ve.data); continue; }
            if (ve.fieldType == kFieldCustom)    { r.customFields = unpackCustom(ve.data); continue; }
            if (ve.fieldType == kFieldCostRates) { r.costRates = unpackCostRates(ve.data); continue; }
            const QString s = QString::fromUtf16(
                reinterpret_cast<const char16_t *>(ve.data.constData()), ve.data.size() / 2);
            if (ve.fieldType == kFieldName) r.name = s;
            else if (ve.fieldType == kFieldInitials) r.initials = s;
            else if (ve.fieldType == kFieldNotes) r.notes = s;
        }
        out.resources.append(r);
    }

    const StreamQuartet asn = StreamQuartet::decode(readQuartet(cf, QStringLiteral("Assignment")), &qerr);
    for (int i = 0; i < asn.fixedRecords.size(); ++i) {
        schedule::Assignment a = unpackAssignment(asn.fixedRecords.at(i));
        for (const auto &ve : asn.varEntries) {
            if (static_cast<int>(ve.itemIndex) != i)
                continue;
            if (ve.fieldType == kFieldBaselines) a.baselines = unpackBaselines(ve.data);
            else if (ve.fieldType == kFieldCustom) a.customFields = unpackCustom(ve.data);
            else if (ve.fieldType == kFieldNotes)
                a.notes = QString::fromUtf16(reinterpret_cast<const char16_t *>(ve.data.constData()),
                                             ve.data.size() / 2);
        }
        out.assignments.append(a);
    }

    const StreamQuartet rel = StreamQuartet::decode(readQuartet(cf, QStringLiteral("Relation")), &qerr);
    for (const QByteArray &rec : rel.fixedRecords)
        out.relations.append(unpackRelation(rec));

    const StreamQuartet cal = StreamQuartet::decode(readQuartet(cf, QStringLiteral("Calendar")), &qerr);
    for (int i = 0; i < cal.fixedRecords.size(); ++i) {
        schedule::Calendar c = unpackCalendar(cal.fixedRecords.at(i));
        for (const auto &ve : cal.varEntries) {
            if (static_cast<int>(ve.itemIndex) != i)
                continue;
            if (ve.fieldType == kFieldCalData) { unpackCalData(ve.data, &c.workingTimes, &c.exceptions); continue; }
            if (ve.fieldType == kFieldName)
                c.name = QString::fromUtf16(reinterpret_cast<const char16_t *>(ve.data.constData()),
                                            ve.data.size() / 2);
        }
        out.calendars.append(c);
    }

    return true;
}

bool DocSerializer::write(const schedule::Project &in, CompoundFile &cf, QString *error) const
{
    // MPP.14 writes the real Microsoft Project format (template-based; see
    // mpp14writer.cpp). MPP.12 still uses the scaffold's own container below.
    if (version() == FormatVersion::Mpp14)
        return writeMpp14(in, cf, error);

    Q_UNUSED(error);
    cf.addStream({ QStringLiteral("Project"), QStringLiteral("Props") },
                 serializeProps(in, version()));

    StreamQuartet tasks;
    tasks.recordSize = kTaskRecordSize;
    for (int i = 0; i < in.tasks.size(); ++i) {
        const schedule::Task &t = in.tasks.at(i);
        tasks.fixedRecords.append(packTask(t));
        if (!t.name.isEmpty())
            tasks.varEntries.append({ static_cast<quint32>(i), kFieldName,
                                      QByteArray(reinterpret_cast<const char *>(t.name.utf16()),
                                                 t.name.size() * 2) });
        if (!t.wbs.isEmpty())
            tasks.varEntries.append({ static_cast<quint32>(i), kFieldWbs,
                                      QByteArray(reinterpret_cast<const char *>(t.wbs.utf16()),
                                                 t.wbs.size() * 2) });
        if (!t.notes.isEmpty())
            tasks.varEntries.append({ static_cast<quint32>(i), kFieldNotes,
                                      QByteArray(reinterpret_cast<const char *>(t.notes.utf16()),
                                                 t.notes.size() * 2) });
        if (!t.baselines.isEmpty())
            tasks.varEntries.append({ static_cast<quint32>(i), kFieldBaselines, packBaselines(t.baselines) });
        if (!t.customFields.isEmpty())
            tasks.varEntries.append({ static_cast<quint32>(i), kFieldCustom, packCustom(t.customFields) });
        const bool hasExtra = t.actualStart.isValid() || t.actualFinish.isValid()
            || t.actualDurationMillis != 0 || t.actualWorkMillis != 0
            || t.evm != schedule::EarnedValue()
            || t.manual || t.effortDriven || t.taskType != 0 || t.priority != 500
            || t.deadline.isValid();
        if (hasExtra)
            tasks.varEntries.append({ static_cast<quint32>(i), kFieldTaskExtra, packTaskExtra(t) });
    }
    writeQuartet(cf, QStringLiteral("Task"), tasks.encode());

    StreamQuartet res;
    res.recordSize = kResourceRecordSize;
    for (int i = 0; i < in.resources.size(); ++i) {
        const schedule::Resource &r = in.resources.at(i);
        res.fixedRecords.append(packResource(r));
        if (!r.name.isEmpty())
            res.varEntries.append({ static_cast<quint32>(i), kFieldName,
                                    QByteArray(reinterpret_cast<const char *>(r.name.utf16()),
                                               r.name.size() * 2) });
        if (!r.initials.isEmpty())
            res.varEntries.append({ static_cast<quint32>(i), kFieldInitials,
                                    QByteArray(reinterpret_cast<const char *>(r.initials.utf16()),
                                               r.initials.size() * 2) });
        if (!r.notes.isEmpty())
            res.varEntries.append({ static_cast<quint32>(i), kFieldNotes,
                                    QByteArray(reinterpret_cast<const char *>(r.notes.utf16()),
                                               r.notes.size() * 2) });
        if (!r.baselines.isEmpty())
            res.varEntries.append({ static_cast<quint32>(i), kFieldBaselines, packBaselines(r.baselines) });
        if (!r.customFields.isEmpty())
            res.varEntries.append({ static_cast<quint32>(i), kFieldCustom, packCustom(r.customFields) });
        if (!r.costRates.isEmpty())
            res.varEntries.append({ static_cast<quint32>(i), kFieldCostRates, packCostRates(r.costRates) });
    }
    writeQuartet(cf, QStringLiteral("Resource"), res.encode());

    StreamQuartet asn;
    asn.recordSize = kAssignmentRecordSize;
    for (int i = 0; i < in.assignments.size(); ++i) {
        const schedule::Assignment &a = in.assignments.at(i);
        asn.fixedRecords.append(packAssignment(a));
        if (!a.notes.isEmpty())
            asn.varEntries.append({ static_cast<quint32>(i), kFieldNotes,
                                    QByteArray(reinterpret_cast<const char *>(a.notes.utf16()),
                                               a.notes.size() * 2) });
        if (!a.baselines.isEmpty())
            asn.varEntries.append({ static_cast<quint32>(i), kFieldBaselines, packBaselines(a.baselines) });
        if (!a.customFields.isEmpty())
            asn.varEntries.append({ static_cast<quint32>(i), kFieldCustom, packCustom(a.customFields) });
    }
    writeQuartet(cf, QStringLiteral("Assignment"), asn.encode());

    StreamQuartet rel;
    rel.recordSize = kRelationRecordSize;
    for (const schedule::Relation &r : in.relations)
        rel.fixedRecords.append(packRelation(r));
    writeQuartet(cf, QStringLiteral("Relation"), rel.encode());

    StreamQuartet cal;
    cal.recordSize = kCalendarRecordSize;
    for (int i = 0; i < in.calendars.size(); ++i) {
        const schedule::Calendar &c = in.calendars.at(i);
        cal.fixedRecords.append(packCalendar(c));
        if (!c.name.isEmpty())
            cal.varEntries.append({ static_cast<quint32>(i), kFieldName,
                                    QByteArray(reinterpret_cast<const char *>(c.name.utf16()),
                                               c.name.size() * 2) });
        if (!c.workingTimes.isEmpty() || !c.exceptions.isEmpty())
            cal.varEntries.append({ static_cast<quint32>(i), kFieldCalData,
                                    packCalData(c.workingTimes, c.exceptions) });
    }
    writeQuartet(cf, QStringLiteral("Calendar"), cal.encode());

    return true;
}
