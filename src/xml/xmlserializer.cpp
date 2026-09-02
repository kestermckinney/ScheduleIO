// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "xml/xmlserializer.h"

#include "codec/mppfieldids.h"
#include "model/duration.h"
#include "model/customfieldlogic.h"
#include "model/workcalendar.h"

#include <QDateTime>
#include <QHash>
#include <QMetaType>
#include <QMultiHash>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <cmath>
#include <algorithm>

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
    // MSPDI carries wall-clock times with no timezone marker, which Qt::ISODate parses
    // as LocalTime -- the spec the whole model uses. Should a file carry a 'Z' anyway,
    // keep its digits rather than its instant, so every date in the model still means
    // the wall clock Project displays.
    const QDateTime parsed = QDateTime::fromString(s.trimmed(), Qt::ISODate);
    // date()/time() read the value in its own spec, so this keeps the digits the file
    // wrote rather than the instant they denote.
    return parsed.isValid() ? QDateTime(parsed.date(), parsed.time()) : QDateTime();
}

QString formatDateTime(const QDateTime &dt)
{
    // MSPDI carries local wall-clock times with no timezone marker. Qt::ISODate would
    // append 'Z' for any UTC-spec value that reached here, which real MS Project
    // exports never do and which makes MS Project shift or reject the value on
    // import. Emit the wall clock unchanged, no 'Z'.
    return dt.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
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

// MSPDI, like native MPP, represents currency values in hundredths of the
// project currency unit. Keep the public model in ordinary currency units.
double parseCurrency(const QString &text)
{
    return text.toDouble() / 100.0;
}

QString formatCurrency(double value)
{
    return formatNumber(value * 100.0);
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
            b.cost = parseCurrency(r.readElementText());
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

schedule::CustomField parseExtendedAttributeDefinition(QXmlStreamReader &r)
{
    schedule::CustomField field;
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"FieldID") field.fieldId = r.readElementText().toInt();
        else if (n == u"FieldName") field.name = r.readElementText();
        else if (n == u"Formula") field.formula = r.readElementText();
        else if (n == u"ValueList") {
            while (r.readNextStartElement()) {
                if (r.name() == u"Value") field.lookupValues.append(r.readElementText());
                else r.skipCurrentElement();
            }
        } else if (n == u"GraphicalIndicators") {
            while (r.readNextStartElement()) {
                if (r.name() != u"GraphicalIndicator") { r.skipCurrentElement(); continue; }
                schedule::CustomField::IndicatorRule rule;
                while (r.readNextStartElement()) {
                    if (r.name() == u"Comparison") rule.comparison = r.readElementText();
                    else if (r.name() == u"Value") rule.value = r.readElementText();
                    else if (r.name() == u"Indicator") rule.indicator = r.readElementText();
                    else r.skipCurrentElement();
                }
                field.graphicalIndicators.append(rule);
            }
        } else r.skipCurrentElement();
    }
    return field;
}

schedule::Task parseTask(QXmlStreamReader &r, QList<schedule::Relation> &relations)
{
    schedule::Task t;
    QDateTime stop;
    QDateTime resume;
    bool resumeValid = false;
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
        else if (n == u"PhysicalPercentComplete")
            t.physicalPercentComplete = r.readElementText().toDouble() / 100.0;
        else if (n == u"EarnedValueMethod")
            t.earnedValueMethod = r.readElementText().toInt() == 1 ? 1 : 0;
        else if (n == u"Milestone")
            t.milestone = r.readElementText().toInt() != 0;
        else if (n == u"Summary")
            t.summary = r.readElementText().toInt() != 0;
        else if (n == u"Recurring")
            t.recurring = r.readElementText().toInt() != 0;
        else if (n == u"Manual")
            t.manual = r.readElementText().toInt() != 0;
        else if (n == u"Active")
            t.active = r.readElementText().toInt() != 0;
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
        else if (n == u"IgnoreResourceCalendar")
            t.ignoreResourceCalendar = r.readElementText().toInt() != 0;
        else if (n == u"LateStart")
            t.lateStart = parseDateTime(r.readElementText());
        else if (n == u"LateFinish")
            t.lateFinish = parseDateTime(r.readElementText());
        else if (n == u"StartVariance")
            t.startVarianceMillis = r.readElementText().toLongLong() * 6000;
        else if (n == u"FinishVariance")
            t.finishVarianceMillis = r.readElementText().toLongLong() * 6000;
        else if (n == u"DurationVariance")
            t.durationVarianceMillis = parseIsoDuration(r.readElementText());
        else if (n == u"WorkVariance")
            t.workVarianceMillis = qRound64(r.readElementText().toDouble());
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
            t.fixedCost = parseCurrency(r.readElementText());
        else if (n == u"FixedCostAccrual")
            t.fixedCostAccrual = r.readElementText().toInt();
        else if (n == u"Cost")
            t.cost = parseCurrency(r.readElementText());
        else if (n == u"ActualCost")
            t.actualCost = parseCurrency(r.readElementText());
        else if (n == u"RemainingCost")
            t.remainingCost = parseCurrency(r.readElementText());
        else if (n == u"CostVariance")
            t.costVariance = parseCurrency(r.readElementText());
        else if (n == u"ActualStart")
            t.actualStart = parseDateTime(r.readElementText());
        else if (n == u"ActualFinish")
            t.actualFinish = parseDateTime(r.readElementText());
        else if (n == u"ActualDuration")
            t.actualDurationMillis = parseIsoDuration(r.readElementText());
        else if (n == u"ActualWork")
            t.actualWorkMillis = parseIsoDuration(r.readElementText());
        else if (n == u"Stop")
            stop = parseDateTime(r.readElementText());
        else if (n == u"Resume")
            resume = parseDateTime(r.readElementText());
        else if (n == u"ResumeValid")
            resumeValid = r.readElementText().toInt() != 0;
        else if (n == u"BCWS")   // Planned Value (PV)
            t.evm.pv = parseCurrency(r.readElementText());
        else if (n == u"BCWP")   // Earned Value (EV)
            t.evm.ev = parseCurrency(r.readElementText());
        else if (n == u"ACWP")   // Actual Cost (AC)
            t.evm.ac = parseCurrency(r.readElementText());
        else if (n == u"CV")
            t.evm.cv = parseCurrency(r.readElementText());
        else if (n == u"SV")
            t.evm.sv = parseCurrency(r.readElementText());
        else if (n == u"CPI")
            t.evm.cpi = r.readElementText().toDouble();
        else if (n == u"SPI")
            t.evm.spi = r.readElementText().toDouble();
        else if (n == u"EAC")
            t.evm.eac = parseCurrency(r.readElementText());
        else if (n == u"TCPI")
            t.evm.tcpi = r.readElementText().toDouble();
        else if (n == u"Notes")
            t.notes = r.readElementText();
        else if (n == u"Hyperlink")
            t.hyperlink = r.readElementText();
        else if (n == u"HyperlinkAddress")
            t.hyperlinkAddress = r.readElementText();
        else if (n == u"HyperlinkSubAddress")
            t.hyperlinkSubAddress = r.readElementText();
        else if (n == u"Baseline")
            t.baselines.append(parseBaseline(r));
        else if (n == u"PredecessorLink")
            parsePredecessorLink(r, t.uniqueId, relations);
        else if (n == u"ExtendedAttribute")
            parseExtendedAttribute(r, t.customFields);
        else
            r.skipCurrentElement();
    }
    // MSPDI represents a task interruption through Stop/Resume. Do not treat
    // ordinary progress anchors as splits unless ResumeValid is explicitly set.
    if (resumeValid && t.start.isValid() && t.finish.isValid()
        && stop > t.start && resume > stop && resume < t.finish) {
        t.segments = { { t.start, stop }, { resume, t.finish } };
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
            cr.standardRate = parseCurrency(r.readElementText());
        else if (n == u"StandardRateFormat")
            cr.standardRateUnit = r.readElementText().toInt();
        else if (n == u"OvertimeRate")
            cr.overtimeRate = parseCurrency(r.readElementText());
        else if (n == u"OvertimeRateFormat")
            cr.overtimeRateUnit = r.readElementText().toInt();
        else if (n == u"CostPerUse")
            cr.costPerUse = parseCurrency(r.readElementText());
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
        else if (n == u"Type")
            res.type = r.readElementText().toInt() == 1
                ? schedule::Resource::Type::Work : schedule::Resource::Type::Material;
        else if (n == u"IsCostResource") {
            if (r.readElementText().toInt() != 0)
                res.type = schedule::Resource::Type::Cost;
        }
        else if (n == u"MaterialLabel")
            res.materialLabel = r.readElementText();
        else if (n == u"IsBudget")
            res.budget = r.readElementText().toInt() != 0;
        else if (n == u"MaxUnits")
            res.maxUnits = r.readElementText().toDouble();
        else if (n == u"CalendarUID")
            res.calendarUniqueId = r.readElementText().toInt();
        else if (n == u"Cost")
            res.cost = parseCurrency(r.readElementText());
        else if (n == u"ActualCost")
            res.actualCost = parseCurrency(r.readElementText());
        else if (n == u"RemainingCost")
            res.remainingCost = parseCurrency(r.readElementText());
        else if (n == u"CostVariance")
            res.costVariance = parseCurrency(r.readElementText());
        else if (n == u"BudgetCost")
            res.budgetCost = parseCurrency(r.readElementText());
        else if (n == u"BudgetWork")
            res.budgetWorkMillis = parseIsoDuration(r.readElementText());
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
        else if (n == u"IsBudget")
            a.budget = r.readElementText().toInt() != 0;
        else if (n == u"BudgetCost")
            a.budgetCost = parseCurrency(r.readElementText());
        else if (n == u"BudgetWork")
            a.budgetWorkMillis = parseIsoDuration(r.readElementText());
        else if (n == u"Units")
            a.units = r.readElementText().toDouble();
        else if (n == u"CostRateTable")
            a.costRateTable = r.readElementText().toInt();
        else if (n == u"RateScale")
            a.variableRateUnits = r.readElementText().toInt();
        else if (n == u"HasFixedRateUnits") {
            if (r.readElementText().toInt() != 0)
                a.variableRateUnits = 0;
        }
        else if (n == u"WorkContour")
            a.workContour = qBound(0, r.readElementText().toInt(), 8);
        else if (n == u"Work")
            a.workMillis = parseIsoDuration(r.readElementText());
        else if (n == u"Start")
            a.start = parseDateTime(r.readElementText());
        else if (n == u"Finish")
            a.finish = parseDateTime(r.readElementText());
        else if (n == u"Stop")
            a.stop = parseDateTime(r.readElementText());
        else if (n == u"Resume")
            a.resume = parseDateTime(r.readElementText());
        else if (n == u"Delay")   // tenths of a minute, like task lag
            a.delayMillis = r.readElementText().toLongLong() * 6000;
        else if (n == u"LevelingDelay")
            a.levelingDelayMillis = r.readElementText().toLongLong() * 6000;
        else if (n == u"ActualWork")
            a.actualWorkMillis = parseIsoDuration(r.readElementText());
        else if (n == u"RemainingWork")
            a.remainingWorkMillis = parseIsoDuration(r.readElementText());
        else if (n == u"OvertimeWork")
            a.overtimeWorkMillis = parseIsoDuration(r.readElementText());
        else if (n == u"ActualOvertimeWork")
            a.actualOvertimeWorkMillis = parseIsoDuration(r.readElementText());
        else if (n == u"RemainingOvertimeWork")
            a.remainingOvertimeWorkMillis = parseIsoDuration(r.readElementText());
        else if (n == u"Cost")
            a.cost = parseCurrency(r.readElementText());
        else if (n == u"ActualCost")
            a.actualCost = parseCurrency(r.readElementText());
        else if (n == u"RemainingCost")
            a.remainingCost = parseCurrency(r.readElementText());
        else if (n == u"CostVariance")
            a.costVariance = parseCurrency(r.readElementText());
        else if (n == u"OvertimeCost")
            a.overtimeCost = parseCurrency(r.readElementText());
        else if (n == u"ActualOvertimeCost")
            a.actualOvertimeCost = parseCurrency(r.readElementText());
        else if (n == u"RemainingOvertimeCost")
            a.remainingOvertimeCost = parseCurrency(r.readElementText());
        else if (n == u"Notes")
            a.notes = r.readElementText();
        else if (n == u"Baseline")
            a.baselines.append(parseBaseline(r));
        else if (n == u"ExtendedAttribute")
            parseExtendedAttribute(r, a.customFields);
        else if (n == u"TimephasedData") {
            schedule::TimephasedValue value;
            while (r.readNextStartElement()) {
                const QStringView child = r.name();
                if (child == u"Type")
                    value.type = r.readElementText().toInt();
                else if (child == u"UID")
                    value.uniqueId = r.readElementText().toInt();
                else if (child == u"Start")
                    value.start = parseDateTime(r.readElementText());
                else if (child == u"Finish")
                    value.finish = parseDateTime(r.readElementText());
                else if (child == u"Unit")
                    value.unit = r.readElementText().toInt();
                else if (child == u"BaselineNumber")
                    value.baselineNumber = r.readElementText().toInt();
                else if (child == u"Value")
                    value.value = r.readElementText();
                else
                    r.skipCurrentElement();
            }
            a.timephasedValues.append(value);
        }
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
    int recurrenceType = 0;
    while (r.readNextStartElement()) {
        const QStringView n = r.name();
        if (n == u"Name")
            ex.name = r.readElementText();
        else if (n == u"Type")
            recurrenceType = r.readElementText().toInt();
        else if (n == u"Period")
            ex.interval = qMax(1, r.readElementText().toInt());
        else if (n == u"Occurrences")
            ex.occurrences = qMax(0, r.readElementText().toInt());
        else if (n == u"DaysOfWeek")
            ex.weekDayMask = quint8(r.readElementText().toUInt());
        else if (n == u"MonthDay")
            ex.dayOfMonth = r.readElementText().toInt();
        else if (n == u"Month")
            ex.month = r.readElementText().toInt();
        else if (n == u"MonthPosition")
            ex.weekPosition = r.readElementText().toInt();
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
        switch (recurrenceType) {
        case 1: ex.recurrence = schedule::CalendarException::Recurrence::Daily; break;
        case 2: ex.recurrence = schedule::CalendarException::Recurrence::YearlyByDate; break;
        case 3: ex.recurrence = schedule::CalendarException::Recurrence::YearlyByPosition; break;
        case 4: ex.recurrence = schedule::CalendarException::Recurrence::MonthlyByDate; break;
        case 5: ex.recurrence = schedule::CalendarException::Recurrence::MonthlyByPosition; break;
        case 6: ex.recurrence = schedule::CalendarException::Recurrence::Weekly; break;
        default: break;
        }
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
    writeText(w, "Cost", formatCurrency(b.cost));
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
            int recurrenceType = 0;
            switch (ex.recurrence) {
            case schedule::CalendarException::Recurrence::Daily: recurrenceType = 1; break;
            case schedule::CalendarException::Recurrence::YearlyByDate: recurrenceType = 2; break;
            case schedule::CalendarException::Recurrence::YearlyByPosition: recurrenceType = 3; break;
            case schedule::CalendarException::Recurrence::MonthlyByDate: recurrenceType = 4; break;
            case schedule::CalendarException::Recurrence::MonthlyByPosition: recurrenceType = 5; break;
            case schedule::CalendarException::Recurrence::Weekly: recurrenceType = 6; break;
            default: break;
            }
            if (recurrenceType) {
                writeText(w, "Type", QString::number(recurrenceType));
                writeText(w, "Period", QString::number(qMax(1, ex.interval)));
                if (ex.occurrences > 0) writeText(w, "Occurrences", QString::number(ex.occurrences));
                if (ex.weekDayMask) writeText(w, "DaysOfWeek", QString::number(ex.weekDayMask));
                if (ex.dayOfMonth > 0) writeText(w, "MonthDay", QString::number(ex.dayOfMonth));
                if (ex.month > 0) writeText(w, "Month", QString::number(ex.month));
                if (ex.weekPosition > 0) writeText(w, "MonthPosition", QString::number(ex.weekPosition));
            }
            writeText(w, "DayWorking", ex.working ? QStringLiteral("1") : QStringLiteral("0"));
            if (ex.working && !ex.workingTimes.isEmpty())
                writeWorkingTimes(w, ex.workingTimes);
            w.writeEndElement();
        }
        w.writeEndElement();
    }
    w.writeEndElement();
}

void writeTask(QXmlStreamWriter &w, const schedule::Project &in, const schedule::Task &t, const QMultiHash<int, const schedule::Relation *> &linksBySucc)
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
    // Manual dates mirror the scheduled dates (auto-scheduled tasks); real
    // MS Project exports always carry these alongside Start/Finish.
    if (t.start.isValid())
        writeText(w, "ManualStart", formatDateTime(t.start));
    if (t.finish.isValid())
        writeText(w, "ManualFinish", formatDateTime(t.finish));
    writeText(w, "Work", formatIsoDuration(t.workMillis));
    writeText(w, "PercentComplete", QString::number(qRound(t.percentComplete * 100.0)));
    writeText(w, "Milestone", t.milestone ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "Summary", t.summary ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "Recurring", t.recurring ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "Manual", t.manual ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "Active", t.active ? QStringLiteral("1") : QStringLiteral("0"));
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
    writeText(w, "IgnoreResourceCalendar",
              t.ignoreResourceCalendar ? QStringLiteral("1") : QStringLiteral("0"));
    if (t.lateStart.isValid())
        writeText(w, "LateStart", formatDateTime(t.lateStart));
    if (t.lateFinish.isValid())
        writeText(w, "LateFinish", formatDateTime(t.lateFinish));
    writeText(w, "StartVariance", QString::number(t.startVarianceMillis / 6000));
    writeText(w, "FinishVariance", QString::number(t.finishVarianceMillis / 6000));
    writeText(w, "WorkVariance", formatNumber(double(t.workVarianceMillis)));
    writeText(w, "FreeSlack", QString::number(t.freeSlackMillis / 6000));
    writeText(w, "TotalSlack", QString::number(t.totalSlackMillis / 6000));
    writeText(w, "Critical", t.critical ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "ConstraintType", QString::number(t.constraintType));
    if (t.constraintDate.isValid())
        writeText(w, "ConstraintDate", formatDateTime(t.constraintDate));
    writeText(w, "FixedCost", formatCurrency(t.fixedCost));
    writeText(w, "FixedCostAccrual", QString::number(t.fixedCostAccrual));
    writeText(w, "Cost", formatCurrency(t.cost));
    writeText(w, "ActualCost", formatCurrency(t.actualCost));
    writeText(w, "RemainingCost", formatCurrency(t.remainingCost));
    writeText(w, "CostVariance", formatCurrency(t.costVariance));
    // Progress anchors. MS Project pins a started task at its ActualStart (and
    // ActualFinish once complete); the Stop/Resume pair records how far actual
    // work has progressed. Without these a completed task is rescheduled to the
    // current date on import. Fall back to the scheduled dates when the model
    // lacks explicit actuals (MS Project always stores an ActualStart once a
    // task has started).
    const int pctComplete = qRound(t.percentComplete * 100.0);
    // Preserve whatever actuals the model carries; only derive a missing one
    // from the scheduled date when progress implies it must exist.
    QDateTime actualStart = t.actualStart;
    if (!actualStart.isValid() && pctComplete > 0)
        actualStart = t.start;
    QDateTime actualFinish = t.actualFinish;
    if (!actualFinish.isValid() && pctComplete >= 100)
        actualFinish = t.finish;
    if (actualStart.isValid())
        writeText(w, "ActualStart", formatDateTime(actualStart));
    if (actualFinish.isValid())
        writeText(w, "ActualFinish", formatDateTime(actualFinish));
    QDateTime stop;
    QDateTime resume;
    bool resumeValid = false;
    if (t.segments.size() >= 2) {
        stop = t.segments.first().finish;
        resume = t.segments.at(1).start;
        resumeValid = stop.isValid() && resume > stop;
    } else if (pctComplete >= 100) {
        stop = actualFinish;
    } else if (pctComplete > 0 && actualStart.isValid()) {
        const schedule::WorkCalendar cal(in, t.calendarUniqueId >= 0 ? t.calendarUniqueId
                                                                      : in.calendarUniqueId);
        stop = t.actualDurationMillis > 0 ? cal.addWork(actualStart, t.actualDurationMillis)
                                          : actualStart;
    }
    if (stop.isValid()) {
        writeText(w, "Stop", formatDateTime(stop));
        writeText(w, "Resume", formatDateTime(resumeValid ? resume : stop));
        writeText(w, "ResumeValid", resumeValid ? QStringLiteral("1") : QStringLiteral("0"));
    }
    writeText(w, "ActualDuration", formatIsoDuration(t.actualDurationMillis));
    writeText(w, "ActualWork", formatIsoDuration(t.actualWorkMillis));
    writeText(w, "BCWS", formatCurrency(t.evm.pv));
    writeText(w, "BCWP", formatCurrency(t.evm.ev));
    writeText(w, "PhysicalPercentComplete",
              QString::number(qRound(t.physicalPercentComplete * 100.0)));
    writeText(w, "EarnedValueMethod", QString::number(t.earnedValueMethod == 1 ? 1 : 0));
    writeText(w, "ACWP", formatCurrency(t.evm.ac));
    writeText(w, "CV", formatCurrency(t.evm.cv));
    writeText(w, "SV", formatCurrency(t.evm.sv));
    writeText(w, "CPI", formatNumber(t.evm.cpi));
    writeText(w, "SPI", formatNumber(t.evm.spi));
    writeText(w, "EAC", formatCurrency(t.evm.eac));
    writeText(w, "TCPI", formatNumber(t.evm.tcpi));
    if (!t.notes.isEmpty())
        writeText(w, "Notes", t.notes);
    if (!t.hyperlink.isEmpty())
        writeText(w, "Hyperlink", t.hyperlink);
    if (!t.hyperlinkAddress.isEmpty())
        writeText(w, "HyperlinkAddress", t.hyperlinkAddress);
    if (!t.hyperlinkSubAddress.isEmpty())
        writeText(w, "HyperlinkSubAddress", t.hyperlinkSubAddress);
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
    writeText(w, "Type", res.type == schedule::Resource::Type::Work
                              ? QStringLiteral("1") : QStringLiteral("0"));
    if (!res.materialLabel.isEmpty())
        writeText(w, "MaterialLabel", res.materialLabel);
    writeText(w, "MaxUnits", formatNumber(res.maxUnits));
    if (res.calendarUniqueId >= 0)
        writeText(w, "CalendarUID", QString::number(res.calendarUniqueId));
    writeText(w, "Cost", formatCurrency(res.cost));
    writeText(w, "ActualCost", formatCurrency(res.actualCost));
    writeText(w, "RemainingCost", formatCurrency(res.remainingCost));
    writeText(w, "CostVariance", formatCurrency(res.costVariance));
    writeText(w, "IsCostResource", res.type == schedule::Resource::Type::Cost
                                           ? QStringLiteral("1") : QStringLiteral("0"));
    writeText(w, "IsBudget", res.budget ? QStringLiteral("1") : QStringLiteral("0"));
    if (res.budgetCost != 0.0) writeText(w, "BudgetCost", formatCurrency(res.budgetCost));
    if (res.budgetWorkMillis != 0) writeText(w, "BudgetWork", formatIsoDuration(res.budgetWorkMillis));
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
            writeText(w, "StandardRate", formatCurrency(cr.standardRate));
            writeText(w, "StandardRateFormat", QString::number(cr.standardRateUnit));
            writeText(w, "OvertimeRate", formatCurrency(cr.overtimeRate));
            writeText(w, "OvertimeRateFormat", QString::number(cr.overtimeRateUnit));
            writeText(w, "CostPerUse", formatCurrency(cr.costPerUse));
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
    writeText(w, "IsBudget", a.budget ? QStringLiteral("1") : QStringLiteral("0"));
    if (a.budgetCost != 0.0)
        writeText(w, "BudgetCost", formatCurrency(a.budgetCost));
    if (a.budgetWorkMillis != 0)
        writeText(w, "BudgetWork", formatIsoDuration(a.budgetWorkMillis));
    writeText(w, "ActualCost", formatCurrency(a.actualCost));
    writeText(w, "ActualWork", formatIsoDuration(a.actualWorkMillis));
    writeText(w, "ActualOvertimeCost", formatCurrency(a.actualOvertimeCost));
    writeText(w, "ActualOvertimeWork", formatIsoDuration(a.actualOvertimeWorkMillis));
    writeText(w, "Cost", formatCurrency(a.cost));
    writeText(w, "CostRateTable", QString::number(a.costRateTable));
    writeText(w, "RateScale", QString::number(a.variableRateUnits));
    writeText(w, "CostVariance", formatCurrency(a.costVariance));
    writeText(w, "OvertimeCost", formatCurrency(a.overtimeCost));
    writeText(w, "OvertimeWork", formatIsoDuration(a.overtimeWorkMillis));
    writeText(w, "Delay", QString::number(a.delayMillis / 6000));
    if (a.finish.isValid())
        writeText(w, "Finish", formatDateTime(a.finish));
    writeText(w, "HasFixedRateUnits", a.variableRateUnits == 0
                                          ? QStringLiteral("1")
                                          : QStringLiteral("0"));
    writeText(w, "LevelingDelay", QString::number(a.levelingDelayMillis / 6000));
    writeText(w, "LevelingDelayFormat", QStringLiteral("7"));
    if (!a.notes.isEmpty())
        writeText(w, "Notes", a.notes);
    writeText(w, "RemainingCost", formatCurrency(a.remainingCost));
    writeText(w, "RemainingOvertimeCost", formatCurrency(a.remainingOvertimeCost));
    writeText(w, "RemainingOvertimeWork", formatIsoDuration(a.remainingOvertimeWorkMillis));
    writeText(w, "RemainingWork", formatIsoDuration(a.remainingWorkMillis));
    if (a.start.isValid())
        writeText(w, "Start", formatDateTime(a.start));
    if (a.stop.isValid())
        writeText(w, "Stop", formatDateTime(a.stop));
    if (a.resume.isValid())
        writeText(w, "Resume", formatDateTime(a.resume));
    writeText(w, "Units", formatNumber(a.units));
    writeText(w, "Work", formatIsoDuration(a.workMillis));
    writeText(w, "WorkContour", QString::number(qBound(0, a.workContour, 8)));
    for (const schedule::Baseline &b : a.baselines)
        writeBaseline(w, b);
    writeExtendedAttributes(w, a.customFields);
    for (const schedule::TimephasedValue &value : a.timephasedValues) {
        w.writeStartElement(QStringLiteral("TimephasedData"));
        writeText(w, "Type", QString::number(value.type));
        writeText(w, "UID", QString::number(value.uniqueId));
        if (value.start.isValid())
            writeText(w, "Start", formatDateTime(value.start));
        if (value.finish.isValid())
            writeText(w, "Finish", formatDateTime(value.finish));
        writeText(w, "Unit", QString::number(value.unit));
        if (value.baselineNumber != 0)
            writeText(w, "BaselineNumber", QString::number(value.baselineNumber));
        writeText(w, "Value", value.value);
        w.writeEndElement();
    }
    w.writeEndElement();
}

// Emit a project-level <ExtendedAttributes> block describing every custom field
// that appears on any entity, so Microsoft Project can associate the values on
// import. (The reader ignores this block; entity values carry the data.)
void writeExtendedAttributeDefs(QXmlStreamWriter &w, const schedule::Project &p)
{
    QHash<int, schedule::CustomField> defs;
    QList<int> order;
    auto collect = [&](const QList<schedule::CustomField> &fields) {
        for (const schedule::CustomField &cf : fields) {
            if (!defs.contains(cf.fieldId)) {
                defs.insert(cf.fieldId, cf);
                order.append(cf.fieldId);
            } else if (!cf.formula.isEmpty() || !cf.lookupValues.isEmpty()
                       || !cf.graphicalIndicators.isEmpty()) {
                schedule::CustomField &stored = defs[cf.fieldId];
                if (!cf.name.isEmpty()) stored.name = cf.name;
                if (!cf.formula.isEmpty()) stored.formula = cf.formula;
                if (!cf.lookupValues.isEmpty()) stored.lookupValues = cf.lookupValues;
                if (!cf.graphicalIndicators.isEmpty()) stored.graphicalIndicators = cf.graphicalIndicators;
            }
        }
    };
    for (const schedule::Task &t : p.tasks)
        collect(t.customFields);
    for (const schedule::Resource &r : p.resources)
        collect(r.customFields);
    for (const schedule::Assignment &a : p.assignments)
        collect(a.customFields);
    collect(p.customFieldDefinitions);
    if (order.isEmpty())
        return;
    w.writeStartElement(QStringLiteral("ExtendedAttributes"));
    for (int fieldId : order) {
        w.writeStartElement(QStringLiteral("ExtendedAttribute"));
        writeText(w, "FieldID", QString::number(fieldId));
        const schedule::CustomField &def = defs[fieldId];
        if (!def.name.isEmpty()) writeText(w, "FieldName", def.name);
        if (!def.formula.isEmpty()) writeText(w, "Formula", def.formula);
        if (!def.lookupValues.isEmpty()) {
            w.writeStartElement(QStringLiteral("ValueList"));
            for (const QString &value : def.lookupValues) writeText(w, "Value", value);
            w.writeEndElement();
        }
        if (!def.graphicalIndicators.isEmpty()) {
            w.writeStartElement(QStringLiteral("GraphicalIndicators"));
            for (const auto &rule : def.graphicalIndicators) {
                w.writeStartElement(QStringLiteral("GraphicalIndicator"));
                writeText(w, "Comparison", rule.comparison);
                writeText(w, "Value", rule.value.toString());
                writeText(w, "Indicator", rule.indicator);
                w.writeEndElement();
            }
            w.writeEndElement();
        }
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
        else if (n == u"ScheduleFromStart")
            out.scheduleFromStart = r.readElementText().toInt() != 0;
        else if (n == u"MultipleCriticalPaths")
            out.multipleCriticalPaths = r.readElementText().toInt() != 0;
        else if (n == u"StatusDate")
            out.statusDate = parseDateTime(r.readElementText());
        else if (n == u"CalendarUID")
            out.calendarUniqueId = r.readElementText().toInt();
        // ---- Project options (File > Options). Kept symmetric with the writer
        // block further down so real MS Project exports round-trip losslessly.
        else if (n == u"NewTasksAreManual")
            out.newTasksManual = r.readElementText().toInt() != 0;
        else if (n == u"NewTaskStartDate")
            out.newTaskStartIsProjectStart = r.readElementText().toInt() == 0;
        else if (n == u"DefaultTaskType")
            out.defaultTaskType = r.readElementText().toInt();
        else if (n == u"DurationFormat")
            out.defaultDurationUnits = r.readElementText().toInt();
        else if (n == u"WorkFormat")
            out.defaultWorkUnits = r.readElementText().toInt();
        else if (n == u"NewTasksEffortDriven")
            out.newTasksEffortDriven = r.readElementText().toInt() != 0;
        else if (n == u"Autolink")
            out.autoLinkTasks = r.readElementText().toInt() != 0;
        else if (n == u"SplitsInProgressTasks")
            out.splitInProgressTasks = r.readElementText().toInt() != 0;
        else if (n == u"HonorConstraints")
            out.honorConstraints = r.readElementText().toInt() != 0;
        else if (n == u"CriticalSlackLimit")
            out.criticalSlackLimit = r.readElementText().toInt();
        else if (n == u"WeekStartDay")
            out.weekStartDay = r.readElementText().toInt();
        else if (n == u"FYStartDate")
            out.fiscalYearStartMonth = r.readElementText().toInt();
        else if (n == u"FiscalYearStart")
            out.fiscalYearUsesStartYear = r.readElementText().toInt() != 0;
        else if (n == u"DefaultStartTime")
            out.defaultStartTime = parseTime(r.readElementText());
        else if (n == u"DefaultFinishTime")
            out.defaultEndTime = parseTime(r.readElementText());
        else if (n == u"MinutesPerDay")
            out.minutesPerDay = r.readElementText().toInt();
        else if (n == u"MinutesPerWeek")
            out.minutesPerWeek = r.readElementText().toInt();
        else if (n == u"DaysPerMonth")
            out.daysPerMonth = r.readElementText().toInt();
        else if (n == u"MoveCompletedEndsBack")
            out.moveCompletedEndsBack = r.readElementText().toInt() != 0;
        else if (n == u"MoveRemainingStartsBack")
            out.moveRemainingStartsBack = r.readElementText().toInt() != 0;
        else if (n == u"MoveRemainingStartsForward")
            out.moveRemainingStartsForward = r.readElementText().toInt() != 0;
        else if (n == u"MoveCompletedEndsForward")
            out.moveCompletedEndsForward = r.readElementText().toInt() != 0;
        else if (n == u"TaskUpdatesResource")
            out.statusUpdatesResource = r.readElementText().toInt() != 0;
        else if (n == u"CurrencySymbol")
            out.currencySymbol = r.readElementText();
        else if (n == u"CurrencySymbolPosition")
            out.currencySymbolPosition = r.readElementText().toInt();
        else if (n == u"CurrencyDigits")
            out.currencyDigits = r.readElementText().toInt();
        else if (n == u"CurrencyCode")
            out.currencyCode = r.readElementText();
        else if (n == u"DefaultStandardRate")
            out.defaultStandardRate = parseCurrency(r.readElementText());
        else if (n == u"DefaultOvertimeRate")
            out.defaultOvertimeRate = parseCurrency(r.readElementText());
        else if (n == u"DefaultFixedCostAccrual")
            out.defaultFixedCostAccrual = r.readElementText().toInt();
        else if (n == u"EarnedValueMethod")
            out.defaultEarnedValueMethod = r.readElementText().toInt();
        else if (n == u"BaselineForEarnedValueCalculation")
            out.baselineForEarnedValue = r.readElementText().toInt();
        else if (n == u"ShowProjectSummaryTask")
            out.showProjectSummaryTask = r.readElementText().toInt() != 0;
        else if (n == u"ExtendedAttributes") {
            while (r.readNextStartElement()) {
                if (r.name() == u"ExtendedAttribute")
                    out.customFieldDefinitions.append(parseExtendedAttributeDefinition(r));
                else r.skipCurrentElement();
            }
        }
        else if (n == u"BudgetCost")
            out.budgetCost = parseCurrency(r.readElementText());
        else if (n == u"BudgetWork")
            out.budgetWorkMillis = parseIsoDuration(r.readElementText());
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
    for (schedule::Task &task : out.tasks) {
        for (const schedule::CustomField &definition : out.customFieldDefinitions) {
            auto it = std::find_if(task.customFields.begin(), task.customFields.end(),
                [&](const schedule::CustomField &field) { return field.fieldId == definition.fieldId; });
            if (it == task.customFields.end()) {
                if (!definition.formula.isEmpty()) task.customFields.append(definition);
            } else {
                it->formula = definition.formula;
                it->lookupValues = definition.lookupValues;
                it->graphicalIndicators = definition.graphicalIndicators;
                if (!definition.name.isEmpty()) it->name = definition.name;
            }
        }
    }
    out.customFieldDefinitions.erase(
        std::remove_if(out.customFieldDefinitions.begin(), out.customFieldDefinitions.end(),
            [](const schedule::CustomField &field) {
                return field.formula.isEmpty() && field.lookupValues.isEmpty()
                    && field.graphicalIndicators.isEmpty();
            }), out.customFieldDefinitions.end());
    schedule::CustomFieldLogic::recalculate(out);
    return true;
}

QByteArray write(const schedule::Project &original, QString *error)
{
    Q_UNUSED(error);
    // Like the .mpp writer: real MS Project exports carry a derived calendar
    // per resource (the resource's CalendarUID points at it), so synthesize
    // them on a working copy for resources holding plain base references.
    schedule::Project inCopy = original;
    schedule::materializeResourceCalendars(inCopy);
    const schedule::Project &in = inCopy;

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
    writeText(w, "ScheduleFromStart", in.scheduleFromStart ? QStringLiteral("1")
                                                            : QStringLiteral("0"));
    writeText(w, "MultipleCriticalPaths", in.multipleCriticalPaths
              ? QStringLiteral("1") : QStringLiteral("0"));
    if (in.startDate.isValid())
        writeText(w, "StartDate", formatDateTime(in.startDate));
    if (in.finishDate.isValid())
        writeText(w, "FinishDate", formatDateTime(in.finishDate));
    if (in.statusDate.isValid()) {
        writeText(w, "StatusDate", formatDateTime(in.statusDate));
        // The "as-of" date MS Project pins reported actuals against; without it
        // MS Project falls back to today and moves completed work.
        writeText(w, "CurrentDate", formatDateTime(in.statusDate));
    }
    if (in.calendarUniqueId >= 0)
        writeText(w, "CalendarUID", QString::number(in.calendarUniqueId));
    if (in.budgetCost != 0.0) writeText(w, "BudgetCost", formatCurrency(in.budgetCost));
    if (in.budgetWorkMillis != 0) writeText(w, "BudgetWork", formatIsoDuration(in.budgetWorkMillis));

    // ---- Project options (File > Options). Emitted from the model, symmetric
    // with the reader block above -- MS Project's own exports always carry these
    // elements, and re-deriving them here would break round-tripping of a file
    // whose stored values differ from what its calendar implies.
    const auto boolText = [](bool v) { return v ? QStringLiteral("1") : QStringLiteral("0"); };
    // Schedule
    writeText(w, "NewTasksAreManual", boolText(in.newTasksManual));
    writeText(w, "NewTaskStartDate", in.newTaskStartIsProjectStart ? QStringLiteral("0")
                                                                   : QStringLiteral("1"));
    writeText(w, "DefaultTaskType", QString::number(in.defaultTaskType));
    writeText(w, "DurationFormat", QString::number(in.defaultDurationUnits));
    writeText(w, "WorkFormat", QString::number(in.defaultWorkUnits));
    writeText(w, "NewTasksEffortDriven", boolText(in.newTasksEffortDriven));
    writeText(w, "Autolink", boolText(in.autoLinkTasks));
    writeText(w, "SplitsInProgressTasks", boolText(in.splitInProgressTasks));
    writeText(w, "HonorConstraints", boolText(in.honorConstraints));
    writeText(w, "CriticalSlackLimit", QString::number(in.criticalSlackLimit));
    // Calendar
    writeText(w, "WeekStartDay", QString::number(in.weekStartDay));
    writeText(w, "FYStartDate", QString::number(in.fiscalYearStartMonth));
    writeText(w, "FiscalYearStart", boolText(in.fiscalYearUsesStartYear));
    writeText(w, "DefaultStartTime", in.defaultStartTime.toString(QStringLiteral("HH:mm:ss")));
    writeText(w, "DefaultFinishTime", in.defaultEndTime.toString(QStringLiteral("HH:mm:ss")));
    writeText(w, "MinutesPerDay", QString::number(in.minutesPerDay));
    writeText(w, "MinutesPerWeek", QString::number(in.minutesPerWeek));
    writeText(w, "DaysPerMonth", QString::number(in.daysPerMonth));
    // Calculation options for this project
    writeText(w, "MoveCompletedEndsBack", boolText(in.moveCompletedEndsBack));
    writeText(w, "MoveRemainingStartsBack", boolText(in.moveRemainingStartsBack));
    writeText(w, "MoveRemainingStartsForward", boolText(in.moveRemainingStartsForward));
    writeText(w, "MoveCompletedEndsForward", boolText(in.moveCompletedEndsForward));
    writeText(w, "TaskUpdatesResource", boolText(in.statusUpdatesResource));
    // Financial
    if (!in.currencySymbol.isEmpty())
        writeText(w, "CurrencySymbol", in.currencySymbol);
    writeText(w, "CurrencySymbolPosition", QString::number(in.currencySymbolPosition));
    writeText(w, "CurrencyDigits", QString::number(in.currencyDigits));
    if (!in.currencyCode.isEmpty())
        writeText(w, "CurrencyCode", in.currencyCode);
    if (in.defaultStandardRate != 0.0)
        writeText(w, "DefaultStandardRate", formatCurrency(in.defaultStandardRate));
    if (in.defaultOvertimeRate != 0.0)
        writeText(w, "DefaultOvertimeRate", formatCurrency(in.defaultOvertimeRate));
    writeText(w, "DefaultFixedCostAccrual", QString::number(in.defaultFixedCostAccrual));
    writeText(w, "EarnedValueMethod", QString::number(in.defaultEarnedValueMethod));
    writeText(w, "BaselineForEarnedValueCalculation", QString::number(in.baselineForEarnedValue));
    writeText(w, "ShowProjectSummaryTask", boolText(in.showProjectSummaryTask));

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
        writeTask(w, in, t, linksBySucc);
    w.writeEndElement();

    // MSPDI requires unique resource IDs; projects whose resources never got
    // one (all id 0) are renumbered by position, like MS Project's sheet order.
    // The "Unassigned" pseudo resource (uid 0) legitimately carries id 0 and is
    // left alone.
    bool renumber = false;
    QSet<int> ids;
    for (const schedule::Resource &res : in.resources) {
        if (res.uniqueId <= 0)
            continue;
        if (res.id <= 0 || ids.contains(res.id)) { renumber = true; break; }
        ids.insert(res.id);
    }
    w.writeStartElement(QStringLiteral("Resources"));
    int nextId = 1;
    for (const schedule::Resource &r : in.resources) {
        schedule::Resource res = r;
        if (renumber && res.uniqueId > 0)
            res.id = nextId++;
        writeResource(w, res);
    }
    w.writeEndElement();

    // Tasks that gained a real resource keep no "unassigned" placeholder row
    // (negative resource uid) in MS Project; a stale one left over from an old
    // file would double-count its work on import.
    QSet<int> tasksWithRealAssignment;
    for (const schedule::Assignment &a : in.assignments)
        if (a.resourceUniqueId >= 0)
            tasksWithRealAssignment.insert(a.taskUniqueId);
    QHash<int, const schedule::Task *> taskByUid;
    for (const schedule::Task &t : in.tasks)
        taskByUid.insert(t.uniqueId, &t);
    w.writeStartElement(QStringLiteral("Assignments"));
    for (const schedule::Assignment &a : in.assignments) {
        if (a.resourceUniqueId < 0) {
            if (tasksWithRealAssignment.contains(a.taskUniqueId))
                continue;
            // A lone placeholder mirrors its task: MS Project carries the task's
            // Work on it when one was entered, otherwise duration x units, with
            // the task's dates. Stale work left in an old file would make
            // MS Project re-derive the task's duration from it on import.
            schedule::Assignment fixed = a;
            if (const schedule::Task *t = taskByUid.value(a.taskUniqueId)) {
                const double units = fixed.units > 0 ? fixed.units : 1.0;
                fixed.workMillis = t->workMillis > 0
                    ? t->workMillis
                    : qint64(std::llround(double(t->durationMillis) * units));
                fixed.remainingWorkMillis =
                    qMax<qint64>(0, fixed.workMillis - fixed.actualWorkMillis);
                fixed.start = t->start;
                fixed.finish = t->finish;
            }
            writeAssignment(w, fixed);
            continue;
        }
        writeAssignment(w, a);
    }
    w.writeEndElement();

    w.writeEndElement();   // Project
    w.writeEndDocument();
    return bytes;
}

} // namespace XmlSerializer
