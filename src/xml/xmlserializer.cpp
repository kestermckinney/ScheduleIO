// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "xml/xmlserializer.h"

#include "codec/mppfieldids.h"
#include "model/duration.h"

#include <QDateTime>
#include <QHash>
#include <QMetaType>
#include <QMultiHash>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <cmath>

// The MSPDI namespace every element lives in.
static const QString kMspdiNs = QStringLiteral("http://schemas.microsoft.com/project");

namespace {

// ---- value conversions ------------------------------------------------------

// MSPDI carries an ISO-8601 duration (e.g. "PT640H0M0S", sometimes "P2DT3H...").
// The model normalises every duration/work value to milliseconds.
qint64 parseIsoDuration(const QString &s)
{
    if (s.isEmpty())
        return 0;
    static const QRegularExpression re(
        QStringLiteral("^(-?)P(?:(\\d+)D)?T(\\d+)H(\\d+)M(\\d+(?:\\.\\d+)?)S$"));
    const QRegularExpressionMatch m = re.match(s.trimmed());
    if (!m.hasMatch())
        return 0;
    const qint64 sign = m.captured(1) == QLatin1String("-") ? -1 : 1;
    const qint64 days = m.captured(2).toLongLong();
    const qint64 hours = m.captured(3).toLongLong();
    const qint64 minutes = m.captured(4).toLongLong();
    const double seconds = m.captured(5).toDouble();
    const qint64 ms = days * 86400000LL + hours * 3600000LL + minutes * 60000LL
        + static_cast<qint64>(std::llround(seconds * 1000.0));
    return sign * ms;
}

QString formatIsoDuration(qint64 ms)
{
    QString sign;
    if (ms < 0) {
        sign = QStringLiteral("-");
        ms = -ms;
    }
    const qint64 hours = ms / 3600000LL;
    qint64 rem = ms % 3600000LL;
    const qint64 minutes = rem / 60000LL;
    rem %= 60000LL;
    const qint64 seconds = rem / 1000LL;
    const qint64 millis = rem % 1000LL;
    QString secStr = QString::number(seconds);
    if (millis != 0) {
        // MSPDI carries sub-second precision (e.g. "PT9H0M1.62S"); keep it so the
        // value round-trips exactly. Trim trailing zeros for a canonical form.
        secStr = QString::number(seconds + millis / 1000.0, 'f', 3);
        while (secStr.endsWith(QLatin1Char('0')))
            secStr.chop(1);
        if (secStr.endsWith(QLatin1Char('.')))
            secStr.chop(1);
    }
    return QStringLiteral("%1PT%2H%3M%4S").arg(sign).arg(hours).arg(minutes).arg(secStr);
}

QDateTime parseDateTime(const QString &s)
{
    return QDateTime::fromString(s.trimmed(), Qt::ISODate);
}

QString formatDateTime(const QDateTime &dt)
{
    return dt.toString(Qt::ISODate);
}

// Whole numbers print without a decimal point; everything else round-trips at
// full double precision so read(write(x)) == read(x).
QString formatNumber(double v)
{
    if (std::floor(v) == v && std::fabs(v) < 1e15)
        return QString::number(static_cast<qint64>(v));
    // Shortest decimal that still parses back to exactly v: clean output (like
    // MS Project's own) while guaranteeing read(write(x)) == read(x).
    for (int prec = 1; prec < 17; ++prec) {
        const QString s = QString::number(v, 'g', prec);
        if (s.toDouble() == v)
            return s;
    }
    return QString::number(v, 'g', 17);
}

QTime parseTime(const QString &s)
{
    return QTime::fromString(s.trimmed(), QStringLiteral("HH:mm:ss"));
}

QString formatTime(const QTime &t)
{
    return t.toString(QStringLiteral("HH:mm:ss"));
}

// ---- custom ("extended") field plumbing ------------------------------------

// Resolve a full MPP field id to its definition (name + decode kind), so an XML
// <ExtendedAttribute> value can be stored in the model's natural Qt type and
// written back the same way the binary reader would have produced it.
const MppFieldIds::CustomFieldDef *customFieldDef(int fieldId)
{
    const quint16 high = static_cast<quint16>((fieldId >> 16) & 0xFFFF);
    const quint16 index = static_cast<quint16>(fieldId & 0xFFFF);
    const QVector<MppFieldIds::CustomFieldDef> *defs = nullptr;
    if (high == MppFieldIds::kTaskHigh)
        defs = &MppFieldIds::taskCustomFields();
    else if (high == MppFieldIds::kResourceHigh)
        defs = &MppFieldIds::resourceCustomFields();
    else if (high == MppFieldIds::kAssignmentHigh)
        defs = &MppFieldIds::assignmentCustomFields();
    if (!defs)
        return nullptr;
    for (const MppFieldIds::CustomFieldDef &d : *defs) {
        if (d.index == index)
            return &d;
    }
    return nullptr;
}

QVariant customValueFromString(const MppFieldIds::CustomFieldDef *def, const QString &raw)
{
    using MppFieldIds::FieldKind;
    if (!def)
        return raw;   // unknown field: keep the literal text
    switch (def->kind) {
    case FieldKind::String:
        return raw;
    case FieldKind::Number:
    case FieldKind::Currency:
        return raw.toDouble();
    case FieldKind::DateTime:
        return parseDateTime(raw);
    case FieldKind::Duration:
        return QVariant::fromValue<qint64>(parseIsoDuration(raw));
    case FieldKind::Bool:
        return raw == QLatin1String("1") || raw.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
    }
    return raw;
}

QString customValueToString(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::QString:
        return v.toString();
    case QMetaType::Double:
        return formatNumber(v.toDouble());
    case QMetaType::LongLong:
    case QMetaType::Int:
        return formatIsoDuration(v.toLongLong());   // qint64 == a duration in ms
    case QMetaType::Bool:
        return v.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    case QMetaType::QDateTime:
        return formatDateTime(v.toDateTime());
    default:
        return v.toString();
    }
}

// =========================== reading ========================================

schedule::Baseline parseBaseline(QXmlStreamReader &r)
{
    schedule::Baseline b;
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"Number")
            b.number = r.readElementText().toInt();
        else if (n == u"Cost")
            b.cost = r.readElementText().toDouble();
        else if (n == u"Work")
            b.workMillis = parseIsoDuration(r.readElementText());
        else if (n == u"Duration")
            b.durationMillis = parseIsoDuration(r.readElementText());
        else if (n == u"Start")
            b.start = parseDateTime(r.readElementText());
        else if (n == u"Finish")
            b.finish = parseDateTime(r.readElementText());
        else
            r.skipCurrentElement();
    }
    return b;
}

void parsePredecessorLink(QXmlStreamReader &r, int successorUid, QList<schedule::Relation> &out)
{
    schedule::Relation rel;
    rel.successorTaskUid = successorUid;
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"PredecessorUID")
            rel.predecessorTaskUid = r.readElementText().toInt();
        else if (n == u"Type")
            rel.type = r.readElementText().toInt();
        else if (n == u"LinkLag")
            rel.lagMillis = r.readElementText().toLongLong() * 6000;   // tenths of a min -> ms
        else if (n == u"LagFormat")
            rel.lagFormat = schedule::Duration::normalizeUnit(r.readElementText().toInt());
        else
            r.skipCurrentElement();
    }
    // Mirror docserializer.cpp's readRealRelations guard: a missing/malformed
    // PredecessorUID leaves predecessorTaskUid at its 0 default, and task uid 0
    // is always the Project Summary Task -- never a legitimate predecessor. A
    // self-referencing link is equally invalid. Both would otherwise reach the
    // MPP14 writer's TBkndCons table and produce a circular-reference row MS
    // Project rejects on reopen.
    if (rel.predecessorTaskUid == 0 || rel.predecessorTaskUid == rel.successorTaskUid)
        return;
    out.append(rel);
}

void parseExtendedAttribute(QXmlStreamReader &r, QList<schedule::CustomField> &out)
{
    int fieldId = 0;
    QString raw;
    bool haveValue = false;
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"FieldID")
            fieldId = r.readElementText().toInt();
        else if (n == u"Value") {
            raw = r.readElementText();
            haveValue = true;
        } else
            r.skipCurrentElement();
    }
    if (fieldId == 0 || !haveValue)
        return;
    const MppFieldIds::CustomFieldDef *def = customFieldDef(fieldId);
    schedule::CustomField cf;
    cf.fieldId = fieldId;
    cf.name = def ? QString::fromLatin1(def->name) : QString();
    cf.value = customValueFromString(def, raw);
    if (cf.value.isValid())
        out.append(cf);
}

schedule::Task parseTask(QXmlStreamReader &r, QList<schedule::Relation> &relations)
{
    schedule::Task t;
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"UID")
            t.uniqueId = r.readElementText().toInt();
        else if (n == u"ID")
            t.id = r.readElementText().toInt();
        else if (n == u"OutlineLevel")
            t.outlineLevel = r.readElementText().toInt();
        else if (n == u"Name")
            t.name = r.readElementText();
        else if (n == u"WBS")
            t.wbs = r.readElementText();
        else if (n == u"Start")
            t.start = parseDateTime(r.readElementText());
        else if (n == u"Finish")
            t.finish = parseDateTime(r.readElementText());
        else if (n == u"Duration")
            t.durationMillis = parseIsoDuration(r.readElementText());
        else if (n == u"DurationFormat")
            t.durationFormat = schedule::Duration::normalizeUnit(r.readElementText().toInt());
        else if (n == u"Work")
            t.workMillis = parseIsoDuration(r.readElementText());
        else if (n == u"LevelingDelay")   // tenths of a minute, like task lag
            t.levelingDelayMillis = r.readElementText().toLongLong() * 6000;
        else if (n == u"PercentComplete")
            t.percentComplete = r.readElementText().toDouble() / 100.0;
        else if (n == u"Milestone")
            t.milestone = r.readElementText().toInt() != 0;
        else if (n == u"Summary")
            t.summary = r.readElementText().toInt() != 0;
        else if (n == u"Manual")
            t.manual = r.readElementText().toInt() != 0;
        else if (n == u"EffortDriven")
            t.effortDriven = r.readElementText().toInt() != 0;
        else if (n == u"Type")   // task type: 0 Fixed Units / 1 Fixed Duration / 2 Fixed Work
            t.taskType = r.readElementText().toInt();
        else if (n == u"Priority")
            t.priority = r.readElementText().toInt();
        else if (n == u"Deadline")
            t.deadline = parseDateTime(r.readElementText());
        else if (n == u"CalendarUID")
            t.calendarUniqueId = r.readElementText().toInt();
        else if (n == u"LateStart")
            t.lateStart = parseDateTime(r.readElementText());
        else if (n == u"LateFinish")
            t.lateFinish = parseDateTime(r.readElementText());
        else if (n == u"TotalSlack")   // tenths of a minute, like task lag
            t.totalSlackMillis = r.readElementText().toLongLong() * 6000;
        else if (n == u"FreeSlack")
            t.freeSlackMillis = r.readElementText().toLongLong() * 6000;
        else if (n == u"Critical")
            t.critical = r.readElementText().toInt() != 0;
        else if (n == u"ConstraintType")
            t.constraintType = r.readElementText().toInt();
        else if (n == u"ConstraintDate")
            t.constraintDate = parseDateTime(r.readElementText());
        else if (n == u"FixedCost")
            t.fixedCost = r.readElementText().toDouble();
        else if (n == u"Cost")
            t.cost = r.readElementText().toDouble();
        else if (n == u"ActualCost")
            t.actualCost = r.readElementText().toDouble();
        else if (n == u"RemainingCost")
            t.remainingCost = r.readElementText().toDouble();
        else if (n == u"CostVariance")
            t.costVariance = r.readElementText().toDouble();
        else if (n == u"ActualStart")
            t.actualStart = parseDateTime(r.readElementText());
        else if (n == u"ActualFinish")
            t.actualFinish = parseDateTime(r.readElementText());
        else if (n == u"ActualDuration")
            t.actualDurationMillis = parseIsoDuration(r.readElementText());
        else if (n == u"ActualWork")
            t.actualWorkMillis = parseIsoDuration(r.readElementText());
        else if (n == u"BCWS")   // Planned Value (PV)
            t.evm.pv = r.readElementText().toDouble();
        else if (n == u"BCWP")   // Earned Value (EV)
            t.evm.ev = r.readElementText().toDouble();
        else if (n == u"ACWP")   // Actual Cost (AC)
            t.evm.ac = r.readElementText().toDouble();
        else if (n == u"CV")
            t.evm.cv = r.readElementText().toDouble();
        else if (n == u"SV")
            t.evm.sv = r.readElementText().toDouble();
        else if (n == u"CPI")
            t.evm.cpi = r.readElementText().toDouble();
        else if (n == u"SPI")
            t.evm.spi = r.readElementText().toDouble();
        else if (n == u"EAC")
            t.evm.eac = r.readElementText().toDouble();
        else if (n == u"TCPI")
            t.evm.tcpi = r.readElementText().toDouble();
        else if (n == u"Notes")
            t.notes = r.readElementText();
        else if (n == u"Baseline")
            t.baselines.append(parseBaseline(r));
        else if (n == u"PredecessorLink")
            parsePredecessorLink(r, t.uniqueId, relations);
        else if (n == u"ExtendedAttribute")
            parseExtendedAttribute(r, t.customFields);
        else
            r.skipCurrentElement();
    }
    return t;
}

schedule::CostRate parseRate(QXmlStreamReader &r)
{
    schedule::CostRate cr;
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"RateTable")
            cr.table = r.readElementText().toInt();
        else if (n == u"RatesFrom")
            cr.startDate = parseDateTime(r.readElementText());
        else if (n == u"RatesTo")
            cr.endDate = parseDateTime(r.readElementText());
        else if (n == u"StandardRate")
            cr.standardRate = r.readElementText().toDouble();
        else if (n == u"StandardRateFormat")
            cr.standardRateUnit = r.readElementText().toInt();
        else if (n == u"OvertimeRate")
            cr.overtimeRate = r.readElementText().toDouble();
        else if (n == u"OvertimeRateFormat")
            cr.overtimeRateUnit = r.readElementText().toInt();
        else if (n == u"CostPerUse")
            cr.costPerUse = r.readElementText().toDouble();
        else
            r.skipCurrentElement();
    }
    return cr;
}

// One row of the Resource Availability grid. An omitted AvailableFrom/AvailableTo
// stays an invalid QDateTime (MS Project's open-ended "NA"), matching the model.
schedule::AvailabilityPeriod parseAvailabilityPeriod(QXmlStreamReader &r)
{
    schedule::AvailabilityPeriod p;
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"AvailableFrom")
            p.startDate = parseDateTime(r.readElementText());
        else if (n == u"AvailableTo")
            p.endDate = parseDateTime(r.readElementText());
        else if (n == u"AvailableUnits")
            p.units = r.readElementText().toDouble();
        else
            r.skipCurrentElement();
    }
    return p;
}

schedule::Resource parseResource(QXmlStreamReader &r)
{
    schedule::Resource res;
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"UID")
            res.uniqueId = r.readElementText().toInt();
        else if (n == u"ID")
            res.id = r.readElementText().toInt();
        else if (n == u"Name")
            res.name = r.readElementText();
        else if (n == u"Initials")
            res.initials = r.readElementText();
        else if (n == u"MaxUnits")
            res.maxUnits = r.readElementText().toDouble();
        else if (n == u"CalendarUID")
            res.calendarUniqueId = r.readElementText().toInt();
        else if (n == u"Cost")
            res.cost = r.readElementText().toDouble();
        else if (n == u"ActualCost")
            res.actualCost = r.readElementText().toDouble();
        else if (n == u"RemainingCost")
            res.remainingCost = r.readElementText().toDouble();
        else if (n == u"CostVariance")
            res.costVariance = r.readElementText().toDouble();
        else if (n == u"Notes")
            res.notes = r.readElementText();
        else if (n == u"Baseline")
            res.baselines.append(parseBaseline(r));
        else if (n == u"ExtendedAttribute")
            parseExtendedAttribute(r, res.customFields);
        else if (n == u"AvailabilityPeriods") {
            while (r.readNextStartElement()) {
                if (r.name() == u"AvailabilityPeriod")
                    res.availabilityTable.append(parseAvailabilityPeriod(r));
                else
                    r.skipCurrentElement();
            }
        } else if (n == u"Rates") {
            while (r.readNextStartElement()) {
                if (r.name() == u"Rate")
                    res.costRates.append(parseRate(r));
                else
                    r.skipCurrentElement();
            }
        } else
            r.skipCurrentElement();
    }
    return res;
}

schedule::Assignment parseAssignment(QXmlStreamReader &r)
{
    schedule::Assignment a;
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"UID")
            a.uniqueId = r.readElementText().toInt();
        else if (n == u"TaskUID")
            a.taskUniqueId = r.readElementText().toInt();
        else if (n == u"ResourceUID")
            a.resourceUniqueId = r.readElementText().toInt();
        else if (n == u"Units")
            a.units = r.readElementText().toDouble();
        else if (n == u"Work")
            a.workMillis = parseIsoDuration(r.readElementText());
        else if (n == u"Start")
            a.start = parseDateTime(r.readElementText());
        else if (n == u"Finish")
            a.finish = parseDateTime(r.readElementText());
        else if (n == u"Delay")   // tenths of a minute, like task lag
            a.delayMillis = r.readElementText().toLongLong() * 6000;
        else if (n == u"LevelingDelay")
            a.levelingDelayMillis = r.readElementText().toLongLong() * 6000;
        else if (n == u"ActualWork")
            a.actualWorkMillis = parseIsoDuration(r.readElementText());
        else if (n == u"RemainingWork")
            a.remainingWorkMillis = parseIsoDuration(r.readElementText());
        else if (n == u"Cost")
            a.cost = r.readElementText().toDouble();
        else if (n == u"ActualCost")
            a.actualCost = r.readElementText().toDouble();
        else if (n == u"RemainingCost")
            a.remainingCost = r.readElementText().toDouble();
        else if (n == u"CostVariance")
            a.costVariance = r.readElementText().toDouble();
        else if (n == u"Notes")
            a.notes = r.readElementText();
        else if (n == u"Baseline")
            a.baselines.append(parseBaseline(r));
        else if (n == u"ExtendedAttribute")
            parseExtendedAttribute(r, a.customFields);
        else
            r.skipCurrentElement();
    }
    return a;
}

QList<schedule::TimeRange> parseWorkingTimes(QXmlStreamReader &r)
{
    QList<schedule::TimeRange> times;
    while (r.readNextStartElement()) {
        if (r.name() == u"WorkingTime") {
            schedule::TimeRange range;
            while (r.readNextStartElement()) {
                const QStringView n = r.name();
                if (n == u"FromTime")
                    range.start = parseTime(r.readElementText());
                else if (n == u"ToTime")
                    range.end = parseTime(r.readElementText());
                else
                    r.skipCurrentElement();
            }
            if (range.start.isValid() && range.end.isValid())
                times.append(range);
        } else
            r.skipCurrentElement();
    }
    return times;
}

void parseWeekDay(QXmlStreamReader &r, schedule::Calendar &cal)
{
    int dayType = -1;
    bool working = false;
    QList<schedule::TimeRange> times;
    QDate fromDate, toDate;   // only used for legacy (DayType 0) exceptions
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"DayType")
            dayType = r.readElementText().toInt();
        else if (n == u"DayWorking")
            working = r.readElementText().toInt() != 0;
        else if (n == u"WorkingTimes")
            times = parseWorkingTimes(r);
        else if (n == u"TimePeriod") {
            while (r.readNextStartElement()) {
                const QStringView tn = r.name();
                if (tn == u"FromDate")
                    fromDate = parseDateTime(r.readElementText()).date();
                else if (tn == u"ToDate")
                    toDate = parseDateTime(r.readElementText()).date();
                else
                    r.skipCurrentElement();
            }
        } else
            r.skipCurrentElement();
    }

    if (dayType >= 1 && dayType <= 7) {
        const int index = (dayType + 5) % 7;   // DayType 1=Sun..7=Sat -> 0=Mon..6=Sun
        if (working)
            cal.workingDayMask |= static_cast<quint8>(1u << index);
        cal.workingTimes[index] = times;
    } else if (dayType == 0 && fromDate.isValid()) {
        schedule::CalendarException ex;
        ex.fromDate = fromDate;
        ex.toDate = toDate.isValid() ? toDate : fromDate;
        ex.working = working;
        ex.workingTimes = times;
        cal.exceptions.append(ex);
    }
}

void parseException(QXmlStreamReader &r, schedule::Calendar &cal)
{
    schedule::CalendarException ex;
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"Name")
            ex.name = r.readElementText();
        else if (n == u"DayWorking")
            ex.working = r.readElementText().toInt() != 0;
        else if (n == u"WorkingTimes")
            ex.workingTimes = parseWorkingTimes(r);
        else if (n == u"TimePeriod") {
            while (r.readNextStartElement()) {
                const QStringView tn = r.name();
                if (tn == u"FromDate")
                    ex.fromDate = parseDateTime(r.readElementText()).date();
                else if (tn == u"ToDate")
                    ex.toDate = parseDateTime(r.readElementText()).date();
                else
                    r.skipCurrentElement();
            }
        } else
            r.skipCurrentElement();
    }
    if (ex.fromDate.isValid()) {
        if (!ex.toDate.isValid())
            ex.toDate = ex.fromDate;
        cal.exceptions.append(ex);
    }
}

schedule::Calendar parseCalendar(QXmlStreamReader &r)
{
    schedule::Calendar cal;
    cal.workingTimes.clear();
    for (int i = 0; i < 7; ++i)
        cal.workingTimes.append(QList<schedule::TimeRange>());
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"UID")
            cal.uniqueId = r.readElementText().toInt();
        else if (n == u"Name")
            cal.name = r.readElementText();
        else if (n == u"BaseCalendarUID")
            cal.baseCalendarUniqueId = r.readElementText().toInt();
        else if (n == u"WeekDays") {
            while (r.readNextStartElement()) {
                if (r.name() == u"WeekDay")
                    parseWeekDay(r, cal);
                else
                    r.skipCurrentElement();
            }
        } else if (n == u"Exceptions") {
            while (r.readNextStartElement()) {
                if (r.name() == u"Exception")
                    parseException(r, cal);
                else
                    r.skipCurrentElement();
            }
        } else
            r.skipCurrentElement();
    }
    return cal;
}

schedule::Project::FormatVersion versionFromSaveVersion(int sv)
{
    switch (sv) {
    case 12:
        return schedule::Project::FormatVersion::Mpp12;
    case 14:
        return schedule::Project::FormatVersion::Mpp14;
    default:
        return schedule::Project::FormatVersion::Unknown;
    }
}

// =========================== writing ========================================

void writeText(QXmlStreamWriter &w, const char *name, const QString &value)
{
    w.writeTextElement(QString::fromLatin1(name), value);
}

void writeBaseline(QXmlStreamWriter &w, const schedule::Baseline &b)
{
    w.writeStartElement(QStringLiteral("Baseline"));
    writeText(w, "Number", QString::number(b.number));
    if (b.start.isValid())
        writeText(w, "Start", formatDateTime(b.start));
    if (b.finish.isValid())
        writeText(w, "Finish", formatDateTime(b.finish));
    writeText(w, "Duration", formatIsoDuration(b.durationMillis));
    writeText(w, "Work", formatIsoDuration(b.workMillis));
    writeText(w, "Cost", formatNumber(b.cost));
    w.writeEndElement();
}

void writeExtendedAttributes(QXmlStreamWriter &w, const QList<schedule::CustomField> &fields)
{
    for (const schedule::CustomField &cf : fields) {
        w.writeStartElement(QStringLiteral("ExtendedAttribute"));
        writeText(w, "FieldID", QString::number(cf.fieldId));
        writeText(w, "Value", customValueToString(cf.value));
        w.writeEndElement();
    }
}

void writeWorkingTimes(QXmlStreamWriter &w, const QList<schedule::TimeRange> &times)
{
    w.writeStartElement(QStringLiteral("WorkingTimes"));
    for (const schedule::TimeRange &t : times) {
        w.writeStartElement(QStringLiteral("WorkingTime"));
        writeText(w, "FromTime", formatTime(t.start));
        writeText(w, "ToTime", formatTime(t.end));
        w.writeEndElement();
    }
    w.writeEndElement();
}

void writeCalendar(QXmlStreamWriter &w, const schedule::Calendar &cal)
{
    w.writeStartElement(QStringLiteral("Calendar"));
    writeText(w, "UID", QString::number(cal.uniqueId));
    writeText(w, "Name", cal.name);
    writeText(w, "IsBaseCalendar", cal.baseCalendarUniqueId < 0 ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "BaseCalendarUID", QString::number(cal.baseCalendarUniqueId));

    w.writeStartElement(QStringLiteral("WeekDays"));
    for (int dayType = 1; dayType <= 7; ++dayType) {
        const int index = (dayType + 5) % 7;   // 0=Mon..6=Sun
        const bool working = (cal.workingDayMask >> index) & 1u;
        w.writeStartElement(QStringLiteral("WeekDay"));
        writeText(w, "DayType", QString::number(dayType));
        writeText(w, "DayWorking", working ? QStringLiteral("1") : QStringLiteral("0"));
        if (working && index < cal.workingTimes.size() && !cal.workingTimes[index].isEmpty())
            writeWorkingTimes(w, cal.workingTimes[index]);
        w.writeEndElement();
    }
    w.writeEndElement();

    if (!cal.exceptions.isEmpty()) {
        w.writeStartElement(QStringLiteral("Exceptions"));
        for (const schedule::CalendarException &ex : cal.exceptions) {
            w.writeStartElement(QStringLiteral("Exception"));
            w.writeStartElement(QStringLiteral("TimePeriod"));
            writeText(w, "FromDate", formatDateTime(QDateTime(ex.fromDate, QTime(0, 0))));
            writeText(w, "ToDate", formatDateTime(QDateTime(ex.toDate, QTime(0, 0))));
            w.writeEndElement();
            if (!ex.name.isEmpty())
                writeText(w, "Name", ex.name);
            writeText(w, "DayWorking", ex.working ? QStringLiteral("1") : QStringLiteral("0"));
            if (ex.working && !ex.workingTimes.isEmpty())
                writeWorkingTimes(w, ex.workingTimes);
            w.writeEndElement();
        }
        w.writeEndElement();
    }
    w.writeEndElement();
}

void writeTask(QXmlStreamWriter &w, const schedule::Task &t, const QMultiHash<int, const schedule::Relation *> &linksBySucc)
{
    w.writeStartElement(QStringLiteral("Task"));
    writeText(w, "UID", QString::number(t.uniqueId));
    writeText(w, "ID", QString::number(t.id));
    if (!t.name.isEmpty())
        writeText(w, "Name", t.name);
    if (!t.wbs.isEmpty())
        writeText(w, "WBS", t.wbs);
    writeText(w, "OutlineLevel", QString::number(t.outlineLevel));
    if (t.start.isValid())
        writeText(w, "Start", formatDateTime(t.start));
    if (t.finish.isValid())
        writeText(w, "Finish", formatDateTime(t.finish));
    writeText(w, "Duration", formatIsoDuration(t.durationMillis));
    writeText(w, "DurationFormat", QString::number(t.durationFormat));
    writeText(w, "Work", formatIsoDuration(t.workMillis));
    writeText(w, "PercentComplete", QString::number(qRound(t.percentComplete * 100.0)));
    writeText(w, "Milestone", t.milestone ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "Summary", t.summary ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "Manual", t.manual ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "EffortDriven", t.effortDriven ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "Type", QString::number(t.taskType));
    writeText(w, "Priority", QString::number(t.priority));
    if (t.deadline.isValid())
        writeText(w, "Deadline", formatDateTime(t.deadline));
    // Format 8 = elapsed days, what MS Project's own exports carry here.
    writeText(w, "LevelingDelay", QString::number(t.levelingDelayMillis / 6000));
    writeText(w, "LevelingDelayFormat", QStringLiteral("8"));
    if (t.calendarUniqueId >= 0)
        writeText(w, "CalendarUID", QString::number(t.calendarUniqueId));
    if (t.lateStart.isValid())
        writeText(w, "LateStart", formatDateTime(t.lateStart));
    if (t.lateFinish.isValid())
        writeText(w, "LateFinish", formatDateTime(t.lateFinish));
    writeText(w, "FreeSlack", QString::number(t.freeSlackMillis / 6000));
    writeText(w, "TotalSlack", QString::number(t.totalSlackMillis / 6000));
    writeText(w, "Critical", t.critical ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "ConstraintType", QString::number(t.constraintType));
    if (t.constraintDate.isValid())
        writeText(w, "ConstraintDate", formatDateTime(t.constraintDate));
    writeText(w, "FixedCost", formatNumber(t.fixedCost));
    writeText(w, "Cost", formatNumber(t.cost));
    writeText(w, "ActualCost", formatNumber(t.actualCost));
    writeText(w, "RemainingCost", formatNumber(t.remainingCost));
    writeText(w, "CostVariance", formatNumber(t.costVariance));
    if (t.actualStart.isValid())
        writeText(w, "ActualStart", formatDateTime(t.actualStart));
    if (t.actualFinish.isValid())
        writeText(w, "ActualFinish", formatDateTime(t.actualFinish));
    writeText(w, "ActualDuration", formatIsoDuration(t.actualDurationMillis));
    writeText(w, "ActualWork", formatIsoDuration(t.actualWorkMillis));
    writeText(w, "BCWS", formatNumber(t.evm.pv));
    writeText(w, "BCWP", formatNumber(t.evm.ev));
    writeText(w, "ACWP", formatNumber(t.evm.ac));
    writeText(w, "CV", formatNumber(t.evm.cv));
    writeText(w, "SV", formatNumber(t.evm.sv));
    writeText(w, "CPI", formatNumber(t.evm.cpi));
    writeText(w, "SPI", formatNumber(t.evm.spi));
    writeText(w, "EAC", formatNumber(t.evm.eac));
    writeText(w, "TCPI", formatNumber(t.evm.tcpi));
    if (!t.notes.isEmpty())
        writeText(w, "Notes", t.notes);
    for (const schedule::Baseline &b : t.baselines)
        writeBaseline(w, b);
    // Predecessor links live on the successor task in MSPDI.
    const QList<const schedule::Relation *> links = linksBySucc.values(t.uniqueId);
    // values() reverses insertion order; restore document order.
    for (auto it = links.crbegin(); it != links.crend(); ++it) {
        const schedule::Relation *rel = *it;
        w.writeStartElement(QStringLiteral("PredecessorLink"));
        writeText(w, "PredecessorUID", QString::number(rel->predecessorTaskUid));
        writeText(w, "Type", QString::number(rel->type));
        writeText(w, "LinkLag", QString::number(rel->lagMillis / 6000));
        writeText(w, "LagFormat", QString::number(rel->lagFormat));
        w.writeEndElement();
    }
    writeExtendedAttributes(w, t.customFields);
    w.writeEndElement();
}

void writeResource(QXmlStreamWriter &w, const schedule::Resource &res)
{
    w.writeStartElement(QStringLiteral("Resource"));
    writeText(w, "UID", QString::number(res.uniqueId));
    writeText(w, "ID", QString::number(res.id));
    if (!res.name.isEmpty())
        writeText(w, "Name", res.name);
    if (!res.initials.isEmpty())
        writeText(w, "Initials", res.initials);
    writeText(w, "MaxUnits", formatNumber(res.maxUnits));
    if (res.calendarUniqueId >= 0)
        writeText(w, "CalendarUID", QString::number(res.calendarUniqueId));
    writeText(w, "Cost", formatNumber(res.cost));
    writeText(w, "ActualCost", formatNumber(res.actualCost));
    writeText(w, "RemainingCost", formatNumber(res.remainingCost));
    writeText(w, "CostVariance", formatNumber(res.costVariance));
    if (!res.notes.isEmpty())
        writeText(w, "Notes", res.notes);
    for (const schedule::Baseline &b : res.baselines)
        writeBaseline(w, b);
    if (!res.availabilityTable.isEmpty()) {
        w.writeStartElement(QStringLiteral("AvailabilityPeriods"));
        for (const schedule::AvailabilityPeriod &p : res.availabilityTable) {
            w.writeStartElement(QStringLiteral("AvailabilityPeriod"));
            if (p.startDate.isValid())
                writeText(w, "AvailableFrom", formatDateTime(p.startDate));
            if (p.endDate.isValid())
                writeText(w, "AvailableTo", formatDateTime(p.endDate));
            writeText(w, "AvailableUnits", formatNumber(p.units));
            w.writeEndElement();
        }
        w.writeEndElement();
    }
    if (!res.costRates.isEmpty()) {
        w.writeStartElement(QStringLiteral("Rates"));
        for (const schedule::CostRate &cr : res.costRates) {
            w.writeStartElement(QStringLiteral("Rate"));
            if (cr.startDate.isValid())
                writeText(w, "RatesFrom", formatDateTime(cr.startDate));
            if (cr.endDate.isValid())
                writeText(w, "RatesTo", formatDateTime(cr.endDate));
            writeText(w, "RateTable", QString::number(cr.table));
            writeText(w, "StandardRate", formatNumber(cr.standardRate));
            writeText(w, "StandardRateFormat", QString::number(cr.standardRateUnit));
            writeText(w, "OvertimeRate", formatNumber(cr.overtimeRate));
            writeText(w, "OvertimeRateFormat", QString::number(cr.overtimeRateUnit));
            writeText(w, "CostPerUse", formatNumber(cr.costPerUse));
            w.writeEndElement();
        }
        w.writeEndElement();
    }
    writeExtendedAttributes(w, res.customFields);
    w.writeEndElement();
}

void writeAssignment(QXmlStreamWriter &w, const schedule::Assignment &a)
{
    w.writeStartElement(QStringLiteral("Assignment"));
    writeText(w, "UID", QString::number(a.uniqueId));
    writeText(w, "TaskUID", QString::number(a.taskUniqueId));
    writeText(w, "ResourceUID", QString::number(a.resourceUniqueId));
    writeText(w, "ActualCost", formatNumber(a.actualCost));
    writeText(w, "ActualWork", formatIsoDuration(a.actualWorkMillis));
    writeText(w, "Cost", formatNumber(a.cost));
    writeText(w, "CostVariance", formatNumber(a.costVariance));
    writeText(w, "Delay", QString::number(a.delayMillis / 6000));
    if (a.finish.isValid())
        writeText(w, "Finish", formatDateTime(a.finish));
    writeText(w, "LevelingDelay", QString::number(a.levelingDelayMillis / 6000));
    writeText(w, "LevelingDelayFormat", QStringLiteral("7"));
    if (!a.notes.isEmpty())
        writeText(w, "Notes", a.notes);
    writeText(w, "RemainingCost", formatNumber(a.remainingCost));
    writeText(w, "RemainingWork", formatIsoDuration(a.remainingWorkMillis));
    if (a.start.isValid())
        writeText(w, "Start", formatDateTime(a.start));
    writeText(w, "Units", formatNumber(a.units));
    writeText(w, "Work", formatIsoDuration(a.workMillis));
    for (const schedule::Baseline &b : a.baselines)
        writeBaseline(w, b);
    writeExtendedAttributes(w, a.customFields);
    w.writeEndElement();
}

// Emit a project-level <ExtendedAttributes> block describing every custom field
// that appears on any entity, so Microsoft Project can associate the values on
// import. (The reader ignores this block; entity values carry the data.)
void writeExtendedAttributeDefs(QXmlStreamWriter &w, const schedule::Project &p)
{
    QHash<int, QString> defs;   // fieldId -> name, in first-seen order via a list
    QList<int> order;
    auto collect = [&](const QList<schedule::CustomField> &fields) {
        for (const schedule::CustomField &cf : fields) {
            if (!defs.contains(cf.fieldId)) {
                defs.insert(cf.fieldId, cf.name);
                order.append(cf.fieldId);
            }
        }
    };
    for (const schedule::Task &t : p.tasks)
        collect(t.customFields);
    for (const schedule::Resource &r : p.resources)
        collect(r.customFields);
    for (const schedule::Assignment &a : p.assignments)
        collect(a.customFields);
    if (order.isEmpty())
        return;
    w.writeStartElement(QStringLiteral("ExtendedAttributes"));
    for (int fieldId : order) {
        w.writeStartElement(QStringLiteral("ExtendedAttribute"));
        writeText(w, "FieldID", QString::number(fieldId));
        if (!defs[fieldId].isEmpty())
            writeText(w, "FieldName", defs[fieldId]);
        w.writeEndElement();
    }
    w.writeEndElement();
}

} // namespace

namespace XmlSerializer {

bool read(const QByteArray &xml, schedule::Project &out, QString *error)
{
    out = schedule::Project();
    QXmlStreamReader r(xml);

    if (!r.readNextStartElement()) {
        if (error)
            *error = QStringLiteral("empty or malformed XML document");
        return false;
    }
    if (r.name() != u"Project") {
        if (error)
            *error = QStringLiteral("not an MSPDI document (root element is <%1>, expected <Project>)")
                         .arg(r.name().toString());
        return false;
    }

    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"SaveVersion")
            out.formatVersion = versionFromSaveVersion(r.readElementText().toInt());
        else if (n == u"Title")
            out.title = r.readElementText();
        else if (n == u"Author")
            out.author = r.readElementText();
        else if (n == u"StartDate")
            out.startDate = parseDateTime(r.readElementText());
        else if (n == u"FinishDate")
            out.finishDate = parseDateTime(r.readElementText());
        else if (n == u"StatusDate")
            out.statusDate = parseDateTime(r.readElementText());
        else if (n == u"CalendarUID")
            out.calendarUniqueId = r.readElementText().toInt();
        else if (n == u"Calendars") {
            while (r.readNextStartElement()) {
                if (r.name() == u"Calendar")
                    out.calendars.append(parseCalendar(r));
                else
                    r.skipCurrentElement();
            }
        } else if (n == u"Tasks") {
            while (r.readNextStartElement()) {
                if (r.name() == u"Task")
                    out.tasks.append(parseTask(r, out.relations));
                else
                    r.skipCurrentElement();
            }
        } else if (n == u"Resources") {
            while (r.readNextStartElement()) {
                if (r.name() == u"Resource")
                    out.resources.append(parseResource(r));
                else
                    r.skipCurrentElement();
            }
        } else if (n == u"Assignments") {
            while (r.readNextStartElement()) {
                if (r.name() == u"Assignment")
                    out.assignments.append(parseAssignment(r));
                else
                    r.skipCurrentElement();
            }
        } else {
            r.skipCurrentElement();
        }
    }

    if (r.hasError()) {
        if (error)
            *error = QStringLiteral("XML parse error at line %1: %2")
                         .arg(r.lineNumber()).arg(r.errorString());
        return false;
    }
    return true;
}

QByteArray write(const schedule::Project &in, QString *error)
{
    Q_UNUSED(error);
    QByteArray bytes;
    QXmlStreamWriter w(&bytes);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(1);

    w.writeStartDocument(QStringLiteral("1.0"));
    w.writeStartElement(QStringLiteral("Project"));
    w.writeDefaultNamespace(kMspdiNs);

    const int sv = in.formatVersion == schedule::Project::FormatVersion::Unknown
        ? 14 : static_cast<int>(in.formatVersion);
    writeText(w, "SaveVersion", QString::number(sv));
    if (!in.title.isEmpty())
        writeText(w, "Title", in.title);
    if (!in.author.isEmpty())
        writeText(w, "Author", in.author);
    if (in.startDate.isValid())
        writeText(w, "StartDate", formatDateTime(in.startDate));
    if (in.finishDate.isValid())
        writeText(w, "FinishDate", formatDateTime(in.finishDate));
    if (in.statusDate.isValid())
        writeText(w, "StatusDate", formatDateTime(in.statusDate));
    if (in.calendarUniqueId >= 0)
        writeText(w, "CalendarUID", QString::number(in.calendarUniqueId));

    writeExtendedAttributeDefs(w, in);

    if (!in.calendars.isEmpty()) {
        w.writeStartElement(QStringLiteral("Calendars"));
        for (const schedule::Calendar &c : in.calendars)
            writeCalendar(w, c);
        w.writeEndElement();
    }

    // Index predecessor links by successor so each task can emit its own.
    QMultiHash<int, const schedule::Relation *> linksBySucc;
    for (const schedule::Relation &rel : in.relations)
        linksBySucc.insert(rel.successorTaskUid, &rel);

    w.writeStartElement(QStringLiteral("Tasks"));
    for (const schedule::Task &t : in.tasks)
        writeTask(w, t, linksBySucc);
    w.writeEndElement();

    w.writeStartElement(QStringLiteral("Resources"));
    for (const schedule::Resource &res : in.resources)
        writeResource(w, res);
    w.writeEndElement();

    w.writeStartElement(QStringLiteral("Assignments"));
    for (const schedule::Assignment &a : in.assignments)
        writeAssignment(w, a);
    w.writeEndElement();

    w.writeEndElement();   // Project
    w.writeEndDocument();
    return bytes;
}

} // namespace XmlSerializer
