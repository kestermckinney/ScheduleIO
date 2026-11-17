// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Microsoft Project custom field formulas. The expected results the evaluator
// is tested against (tst_customfieldformulas) were read from Project 2016 on
// dpr2hw3 by ScheduleVault/tools/customfield-samples-remote.vbs (case cf-eval).
#include "model/customfieldlogic.h"
#include "model/project.h"
#include "model/task.h"
#include "model/workcalendar.h"
#include "codec/mppfieldids.h"

#include <QHash>
#include <QRegularExpression>
#include <QTimeZone>
#include <QtMath>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace schedule {
namespace {

// ---- values ----------------------------------------------------------------

struct Val {
    enum Kind { Empty, Num, Str, Bool, Date, Err };
    Kind k = Empty;
    double n = 0.0;
    QString s;          // Str text, Err message
    QDateTime d;        // Date; invalid is Project's NA
    bool b = false;

    static Val num(double v) { Val r; r.k = Num; r.n = v; return r; }
    static Val str(const QString &v) { Val r; r.k = Str; r.s = v; return r; }
    static Val boolean(bool v) { Val r; r.k = Bool; r.b = v; return r; }
    static Val date(const QDateTime &v) { Val r; r.k = Date; r.d = v; return r; }
    static Val err(const QString &m) { Val r; r.k = Err; r.s = m; return r; }
};

// Project's NA date, as a number (the value its formulas compare NA against).
constexpr double kNaDate = 4294967295.0;

// Dates as OLE automation days since 1899-12-30, on the wall clock (MPP dates
// carry no zone, so no daylight-saving shift may creep in).
const QDateTime kOleEpoch(QDate(1899, 12, 30), QTime(0, 0), QTimeZone::UTC);

double toOle(const QDateTime &d)
{
    if (!d.isValid()) return kNaDate;
    const QDateTime wall(d.date(), d.time(), QTimeZone::UTC);
    return double(kOleEpoch.msecsTo(wall)) / 86400000.0;
}

QDateTime fromOle(double days)
{
    const QDateTime wall = kOleEpoch.addMSecs(qint64(std::llround(days * 86400000.0)));
    return QDateTime(wall.date(), wall.time());
}

qint64 wallSecs(const QDateTime &d)
{
    return QDateTime(d.date(), d.time(), QTimeZone::UTC).toSecsSinceEpoch();
}

QString fmtNum(double v)
{
    if (v == 0.0) return QStringLiteral("0");
    QString s = QString::number(v, 'g', 15);
    s.replace(QStringLiteral("e+"), QStringLiteral("E+"));
    s.replace(QStringLiteral("e-"), QStringLiteral("E-"));
    return s;
}

QString fmtDate(const QDateTime &d)
{
    if (!d.isValid()) return QStringLiteral("NA");
    const QString date = d.date().toString(QStringLiteral("M/d/yyyy"));
    if (d.time() == QTime(0, 0)) return date;
    return date + QLatin1Char(' ') + d.time().toString(QStringLiteral("h:mm:ss AP"));
}

QString asStr(const Val &v)
{
    switch (v.k) {
    case Val::Num: return fmtNum(v.n);
    case Val::Str: return v.s;
    case Val::Bool: return v.b ? QStringLiteral("True") : QStringLiteral("False");
    case Val::Date: return fmtDate(v.d);
    default: return QString();
    }
}

bool parseNumber(const QString &text, double *out)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return false;
    bool ok = false;
    const double v = t.toDouble(&ok);
    if (ok) *out = v;
    return ok;
}

QDateTime parseDate(const QString &text)
{
    const QString t = text.trimmed();
    if (t.compare(QLatin1String("NA"), Qt::CaseInsensitive) == 0) return QDateTime();
    static const char *const forms[] = {
        "M/d/yyyy h:mm:ss AP", "M/d/yyyy h:mm AP", "M/d/yyyy H:mm:ss", "M/d/yyyy H:mm",
        "M/d/yyyy", "M/d/yy h:mm AP", "M/d/yy", "yyyy-MM-ddTHH:mm:ss", "yyyy-MM-dd HH:mm",
        "yyyy-MM-dd", "MMMM d, yyyy", "MMM d, yyyy", "d MMMM yyyy", "d MMM yyyy" };
    for (const char *f : forms) {
        QDateTime d = QDateTime::fromString(t, QLatin1String(f));
        if (d.isValid()) {
            if (d.date().year() < 1930 && QLatin1String(f).contains(QLatin1String("yy"))
                && !QLatin1String(f).contains(QLatin1String("yyyy")))
                d = d.addYears(100);
            return d;
        }
    }
    return QDateTime();
}

bool asNum(const Val &v, double *out)
{
    switch (v.k) {
    case Val::Num: *out = v.n; return true;
    case Val::Bool: *out = v.b ? -1.0 : 0.0; return true;
    case Val::Date: *out = toOle(v.d); return true;
    case Val::Empty: *out = 0.0; return true;
    case Val::Str: return parseNumber(v.s, out);
    default: return false;
    }
}

bool truth(const Val &v)
{
    if (v.k == Val::Bool) return v.b;
    if (v.k == Val::Str) {
        if (v.s.compare(QLatin1String("True"), Qt::CaseInsensitive) == 0
            || v.s.compare(QLatin1String("Yes"), Qt::CaseInsensitive) == 0) return true;
        if (v.s.compare(QLatin1String("False"), Qt::CaseInsensitive) == 0
            || v.s.compare(QLatin1String("No"), Qt::CaseInsensitive) == 0) return false;
    }
    double n = 0.0;
    return asNum(v, &n) && n != 0.0;
}

bool asDate(const Val &v, QDateTime *out)
{
    if (v.k == Val::Date) { *out = v.d; return true; }
    if (v.k == Val::Str) { *out = parseDate(v.s); return out->isValid() || v.s.trimmed() == QLatin1String("NA"); }
    double n = 0.0;
    if (!asNum(v, &n)) return false;
    *out = n >= kNaDate ? QDateTime() : fromOle(n);
    return true;
}

// VBA rounds half to even (Round, CInt, CLng, \ and Mod operands).
double bankers(double v) { return std::nearbyint(v); }

// ---- Project task fields -----------------------------------------------------

using Getter = std::function<Val(const Project &, const Task &)>;
struct FieldDef {
    quint16 index;
    const char *name;
    Getter get;
};

double minutes(qint64 ms) { return double(ms) / 60000.0; }

const Baseline *baseline0(const Task &t)
{
    for (const Baseline &b : t.baselines)
        if (b.number == 0) return &b;
    return nullptr;
}

QString resourceNames(const Project &p, const Task &t)
{
    QStringList names;
    for (const Assignment &a : p.assignments) {
        if (a.taskUniqueId != t.uniqueId) continue;
        for (const Resource &r : p.resources) {
            if (r.uniqueId != a.resourceUniqueId) continue;
            QString n = r.name;
            if (qAbs(a.units - 1.0) > 1e-9)
                n += QStringLiteral("[%1%]").arg(fmtNum(std::round(a.units * 1000.0) / 10.0));
            names.append(n);
        }
    }
    return names.join(QLatin1Char(','));
}

const QVector<FieldDef> &fields()
{
    static const QVector<FieldDef> table = {
        { 0, "Work", [](const Project &, const Task &t) { return Val::num(minutes(t.workMillis)); } },
        { 1, "Baseline Work", [](const Project &, const Task &t) {
              const Baseline *b = baseline0(t); return Val::num(b ? minutes(b->workMillis) : 0.0); } },
        { 2, "Actual Work", [](const Project &, const Task &t) { return Val::num(minutes(t.actualWorkMillis)); } },
        { 3, "Work Variance", [](const Project &, const Task &t) { return Val::num(minutes(t.workVarianceMillis)); } },
        { 4, "Remaining Work", [](const Project &, const Task &t) {
              return Val::num(minutes(qMax<qint64>(0, t.workMillis - t.actualWorkMillis))); } },
        { 5, "Cost", [](const Project &, const Task &t) { return Val::num(t.cost); } },
        { 6, "Baseline Cost", [](const Project &, const Task &t) {
              const Baseline *b = baseline0(t); return Val::num(b ? b->cost : 0.0); } },
        { 7, "Actual Cost", [](const Project &, const Task &t) { return Val::num(t.actualCost); } },
        { 8, "Fixed Cost", [](const Project &, const Task &t) { return Val::num(t.fixedCost); } },
        { 9, "Cost Variance", [](const Project &, const Task &t) { return Val::num(t.costVariance); } },
        { 10, "Remaining Cost", [](const Project &, const Task &t) { return Val::num(t.remainingCost); } },
        { 11, "BCWP", [](const Project &, const Task &t) { return Val::num(t.evm.ev); } },
        { 12, "BCWS", [](const Project &, const Task &t) { return Val::num(t.evm.pv); } },
        { 13, "SV", [](const Project &, const Task &t) { return Val::num(t.evm.sv); } },
        { 14, "Name", [](const Project &, const Task &t) { return Val::str(t.name); } },
        { 16, "WBS", [](const Project &, const Task &t) { return Val::str(t.wbs); } },
        { 19, "Critical", [](const Project &, const Task &t) { return Val::boolean(t.critical); } },
        { 21, "Free Slack", [](const Project &, const Task &t) { return Val::num(minutes(t.freeSlackMillis)); } },
        { 22, "Total Slack", [](const Project &, const Task &t) { return Val::num(minutes(t.totalSlackMillis)); } },
        { 23, "ID", [](const Project &, const Task &t) { return Val::num(t.id); } },
        { 24, "Milestone", [](const Project &, const Task &t) { return Val::boolean(t.milestone); } },
        { 25, "Priority", [](const Project &, const Task &t) { return Val::num(t.priority); } },
        { 27, "Baseline Duration", [](const Project &, const Task &t) {
              const Baseline *b = baseline0(t); return Val::num(b ? minutes(b->durationMillis) : 0.0); } },
        { 28, "Actual Duration", [](const Project &, const Task &t) { return Val::num(minutes(t.actualDurationMillis)); } },
        { 30, "Duration Variance", [](const Project &, const Task &t) { return Val::num(minutes(t.durationVarianceMillis)); } },
        { 31, "Remaining Duration", [](const Project &, const Task &t) {
              return Val::num(minutes(qMax<qint64>(0, t.durationMillis - t.actualDurationMillis))); } },
        { 32, "% Complete", [](const Project &, const Task &t) { return Val::num(std::round(t.percentComplete * 100.0)); } },
        { 33, "% Work Complete", [](const Project &, const Task &t) {
              return Val::num(t.workMillis > 0 ? std::round(100.0 * double(t.actualWorkMillis) / double(t.workMillis)) : 0.0); } },
        { 41, "Actual Start", [](const Project &, const Task &t) { return Val::date(t.actualStart); } },
        { 42, "Actual Finish", [](const Project &, const Task &t) { return Val::date(t.actualFinish); } },
        { 43, "Baseline Start", [](const Project &, const Task &t) {
              const Baseline *b = baseline0(t); return Val::date(b ? b->start : QDateTime()); } },
        { 44, "Baseline Finish", [](const Project &, const Task &t) {
              const Baseline *b = baseline0(t); return Val::date(b ? b->finish : QDateTime()); } },
        { 45, "Start Variance", [](const Project &, const Task &t) { return Val::num(minutes(t.startVarianceMillis)); } },
        { 46, "Finish Variance", [](const Project &, const Task &t) { return Val::num(minutes(t.finishVarianceMillis)); } },
        { 49, "Resource Names", [](const Project &p, const Task &t) { return Val::str(resourceNames(p, t)); } },
        { 83, "CV", [](const Project &, const Task &t) { return Val::num(t.evm.cv); } },
        { 85, "Outline Level", [](const Project &, const Task &t) { return Val::num(t.outlineLevel); } },
        { 86, "Unique ID", [](const Project &, const Task &t) { return Val::num(t.uniqueId); } },
        { 92, "Summary", [](const Project &, const Task &t) { return Val::boolean(t.summary); } },
        { 120, "ACWP", [](const Project &, const Task &t) { return Val::num(t.evm.ac); } },
        { 437, "Deadline", [](const Project &, const Task &t) { return Val::date(t.deadline); } },
        { 537, "CPI", [](const Project &, const Task &t) { return Val::num(t.evm.cpi); } },
        { 538, "SPI", [](const Project &, const Task &t) { return Val::num(t.evm.spi); } },
        { 541, "EAC", [](const Project &, const Task &t) { return Val::num(t.evm.eac); } },
        { 542, "TCPI", [](const Project &, const Task &t) { return Val::num(t.evm.tcpi); } },
        { 1119, "Physical % Complete", [](const Project &, const Task &t) {
              return Val::num(std::round(t.physicalPercentComplete * 100.0)); } },
        { 1285, "Start", [](const Project &, const Task &t) { return Val::date(t.start); } },
        { 1286, "Finish", [](const Project &, const Task &t) { return Val::date(t.finish); } },
        { 1287, "Duration", [](const Project &, const Task &t) { return Val::num(minutes(t.durationMillis)); } },
    };
    return table;
}

// Other spellings that mean the same field: Project's classic Start/Finish/
// Duration ids, earned-value column titles, and ScheduleVault's old name.
quint16 aliasIndex(quint16 index)
{
    switch (index) {
    case 35: return 1285;
    case 36: return 1286;
    case 29: return 1287;
    default: return index;
    }
}

const FieldDef *fieldByIndex(quint16 index)
{
    index = aliasIndex(index);
    for (const FieldDef &f : fields())
        if (f.index == index) return &f;
    return nullptr;
}

const FieldDef *fieldByName(const QString &name)
{
    for (const FieldDef &f : fields())
        if (name.compare(QLatin1String(f.name), Qt::CaseInsensitive) == 0) return &f;
    static const QHash<QString, quint16> synonyms = {
        { QStringLiteral("percent complete"), 32 }, { QStringLiteral("ev"), 11 },
        { QStringLiteral("pv"), 12 }, { QStringLiteral("ac"), 120 },
        { QStringLiteral("earned value"), 11 }, { QStringLiteral("planned value"), 12 },
        { QStringLiteral("actual cost of work performed"), 120 },
    };
    const auto it = synonyms.constFind(name.toLower());
    return it == synonyms.constEnd() ? nullptr : fieldByIndex(it.value());
}

constexpr quint32 taskId(quint16 index) { return (quint32(MppFieldIds::kTaskHigh) << 16) | index; }
bool isTaskId(quint32 id) { return quint16(id >> 16) == MppFieldIds::kTaskHigh; }

const MppFieldIds::CustomFieldDef *slotDef(quint32 fieldId)
{
    if (!isTaskId(fieldId)) return nullptr;
    for (const MppFieldIds::CustomFieldDef &d : MppFieldIds::taskCustomFields())
        if (d.index == quint16(fieldId & 0xFFFF)) return &d;
    return nullptr;
}

const MppFieldIds::CustomFieldDef *slotByName(const QString &name)
{
    for (const MppFieldIds::CustomFieldDef &d : MppFieldIds::taskCustomFields())
        if (name.compare(QLatin1String(d.name), Qt::CaseInsensitive) == 0) return &d;
    return nullptr;
}

const CustomField *definitionFor(const Project &p, quint32 fieldId, const QString &name)
{
    for (const CustomField &d : p.customFieldDefinitions)
        if ((fieldId && quint32(d.fieldId) == fieldId)
            || (!name.isEmpty() && d.name.compare(name, Qt::CaseInsensitive) == 0))
            return &d;
    return nullptr;
}

MppFieldIds::FieldKind kindOf(quint32 fieldId)
{
    const MppFieldIds::CustomFieldDef *d = slotDef(fieldId);
    return d ? d->kind : MppFieldIds::FieldKind::String;
}

Val customValue(const Task &t, quint32 fieldId, const QString &name)
{
    const CustomField *found = nullptr;
    for (const CustomField &f : t.customFields)
        if ((fieldId && quint32(f.fieldId) == fieldId)
            || (!found && name.compare(f.name, Qt::CaseInsensitive) == 0))
            found = &f;
    const QVariant v = found ? found->value : QVariant();
    using MppFieldIds::FieldKind;
    switch (fieldId ? kindOf(fieldId) : FieldKind::String) {
    case FieldKind::Number:
    case FieldKind::Currency: return Val::num(v.toDouble());
    case FieldKind::Bool: return Val::boolean(v.toBool());
    case FieldKind::DateTime: return Val::date(v.toDateTime());
    case FieldKind::Duration: return Val::num(minutes(v.toLongLong()));
    case FieldKind::String:
    default:
        if (v.typeId() == QMetaType::Double || v.typeId() == QMetaType::LongLong)
            return Val::num(v.toDouble());
        if (v.typeId() == QMetaType::Bool) return Val::boolean(v.toBool());
        if (v.typeId() == QMetaType::QDateTime) return Val::date(v.toDateTime());
        return Val::str(v.toString());
    }
}

// ---- Format() --------------------------------------------------------------

QString formatNumberPattern(double v, QString pattern)
{
    // Sections: positive;negative;zero.
    const QStringList sections = pattern.split(QLatin1Char(';'));
    bool negSection = false;
    if (v < 0 && sections.size() > 1) { pattern = sections.at(1); negSection = true; }
    else if (v == 0 && sections.size() > 2) pattern = sections.at(2);
    else pattern = sections.at(0);

    const bool percent = pattern.contains(QLatin1Char('%'));
    double x = qAbs(v) * (percent ? 100.0 : 1.0);
    int first = -1, last = -1;
    for (int i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern.at(i);
        if (c == QLatin1Char('0') || c == QLatin1Char('#') || c == QLatin1Char('.') || c == QLatin1Char(',')) {
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first < 0) return pattern;
    const QString spec = pattern.mid(first, last - first + 1);
    const int dot = spec.indexOf(QLatin1Char('.'));
    const QString intSpec = dot < 0 ? spec : spec.left(dot);
    const QString fracSpec = dot < 0 ? QString() : spec.mid(dot + 1);
    const bool group = intSpec.contains(QLatin1Char(','));
    const int minInt = int(intSpec.count(QLatin1Char('0')));
    const int maxFrac = int(fracSpec.count(QLatin1Char('0')) + fracSpec.count(QLatin1Char('#')));
    const int minFrac = int(fracSpec.count(QLatin1Char('0')));

    const double scale = std::pow(10.0, maxFrac);
    x = std::floor(x * scale + 0.5) / scale;   // Format rounds half away from zero
    QString digits = QString::number(x, 'f', maxFrac);
    QString ip = maxFrac > 0 ? digits.section(QLatin1Char('.'), 0, 0) : digits;
    QString fp = maxFrac > 0 ? digits.section(QLatin1Char('.'), 1, 1) : QString();
    while (fp.size() > minFrac && fp.endsWith(QLatin1Char('0'))) fp.chop(1);
    if (ip == QLatin1String("0") && minInt == 0) ip.clear();
    while (ip.size() < minInt) ip.prepend(QLatin1Char('0'));
    if (group)
        for (int i = int(ip.size()) - 3; i > 0; i -= 3) ip.insert(i, QLatin1Char(','));
    QString number = ip;
    if (!fp.isEmpty() || (dot >= 0 && minFrac > 0)) number += QLatin1Char('.') + fp;

    auto literal = [](const QString &s) {
        QString out;
        for (int i = 0; i < s.size(); ++i) {
            const QChar c = s.at(i);
            if (c == QLatin1Char('"')) {
                const int end = s.indexOf(QLatin1Char('"'), i + 1);
                out += s.mid(i + 1, (end < 0 ? s.size() : end) - i - 1);
                i = end < 0 ? s.size() : end;
            } else if (c == QLatin1Char('\\') && i + 1 < s.size()) {
                out += s.at(++i);
            } else {
                out += c;
            }
        }
        return out;
    };
    QString out = literal(pattern.left(first)) + number + literal(pattern.mid(last + 1));
    if (v < 0 && !negSection && x != 0.0) out.prepend(QLatin1Char('-'));
    return out;
}

QString formatDatePattern(const QDateTime &d, const QString &pattern)
{
    if (!d.isValid()) return QStringLiteral("NA");
    QString out;
    bool afterHour = false;
    const QDate date = d.date();
    const QTime time = d.time();
    const bool ampm = pattern.contains(QLatin1String("AM/PM"), Qt::CaseInsensitive)
                      || pattern.contains(QLatin1String("A/P"), Qt::CaseInsensitive);
    int i = 0;
    auto run = [&](QChar c) {
        int n = 0;
        while (i + n < pattern.size() && pattern.at(i + n).toLower() == c) ++n;
        return n;
    };
    while (i < pattern.size()) {
        const QChar c = pattern.at(i).toLower();
        if (pattern.mid(i, 5).compare(QLatin1String("AM/PM"), Qt::CaseInsensitive) == 0) {
            const bool upper = pattern.at(i).isUpper();
            const QString s = time.hour() < 12 ? QStringLiteral("AM") : QStringLiteral("PM");
            out += upper ? s : s.toLower();
            i += 5;
        } else if (pattern.mid(i, 3).compare(QLatin1String("A/P"), Qt::CaseInsensitive) == 0) {
            out += time.hour() < 12 ? QLatin1Char('A') : QLatin1Char('P');
            i += 3;
        } else if (c == QLatin1Char('y')) {
            const int n = run(c);
            out += n >= 3 ? QString::number(date.year())
                          : QStringLiteral("%1").arg(date.year() % 100, 2, 10, QLatin1Char('0'));
            i += n;
        } else if (c == QLatin1Char('m')) {
            const int n = run(c);
            if (afterHour && n <= 2) {
                out += n == 2 ? QStringLiteral("%1").arg(time.minute(), 2, 10, QLatin1Char('0'))
                              : QString::number(time.minute());
            } else if (n >= 4) out += QLocale::c().monthName(date.month(), QLocale::LongFormat);
            else if (n == 3) out += QLocale::c().monthName(date.month(), QLocale::ShortFormat);
            else out += n == 2 ? QStringLiteral("%1").arg(date.month(), 2, 10, QLatin1Char('0'))
                               : QString::number(date.month());
            i += n;
        } else if (c == QLatin1Char('d')) {
            const int n = run(c);
            if (n >= 4) out += QLocale::c().dayName(date.dayOfWeek(), QLocale::LongFormat);
            else if (n == 3) out += QLocale::c().dayName(date.dayOfWeek(), QLocale::ShortFormat);
            else out += n == 2 ? QStringLiteral("%1").arg(date.day(), 2, 10, QLatin1Char('0'))
                               : QString::number(date.day());
            i += n;
        } else if (c == QLatin1Char('h')) {
            const int n = run(c);
            int h = time.hour();
            if (ampm) { h %= 12; if (h == 0) h = 12; }
            out += n >= 2 ? QStringLiteral("%1").arg(h, 2, 10, QLatin1Char('0')) : QString::number(h);
            afterHour = true;
            i += n;
            continue;
        } else if (c == QLatin1Char('n') || c == QLatin1Char('s')) {
            const int n = run(c);
            const int v = c == QLatin1Char('n') ? time.minute() : time.second();
            out += n >= 2 ? QStringLiteral("%1").arg(v, 2, 10, QLatin1Char('0')) : QString::number(v);
            i += n;
        } else if (c == QLatin1Char('"')) {
            const int end = pattern.indexOf(QLatin1Char('"'), i + 1);
            out += pattern.mid(i + 1, (end < 0 ? pattern.size() : end) - i - 1);
            i = end < 0 ? int(pattern.size()) : end + 1;
        } else if (c == QLatin1Char('\\') && i + 1 < pattern.size()) {
            out += pattern.at(i + 1);
            i += 2;
        } else {
            out += pattern.at(i);
            ++i;
        }
        if (c != QLatin1Char(':') && !c.isSpace()) afterHour = false;
    }
    return out;
}

QString formatValue(const Project &p, const Val &v, const QString &fmt)
{
    const QString f = fmt.trimmed();
    if (f.isEmpty()) return asStr(v);
    const QString lf = f.toLower();
    double n = 0.0;
    const bool numeric = v.k != Val::Date && asNum(v, &n);
    auto currency = [&](double x) {
        const QString sym = p.currencySymbol.isEmpty() ? QStringLiteral("$") : p.currencySymbol;
        const int digits = p.currencyDigits;
        QString body = formatNumberPattern(qAbs(x), digits > 0
            ? QStringLiteral("#,##0.") + QString(digits, QLatin1Char('0')) : QStringLiteral("#,##0"));
        QString s = (p.currencySymbolPosition == 1 || p.currencySymbolPosition == 3)
                    ? body + (p.currencySymbolPosition == 3 ? QStringLiteral(" ") : QString()) + sym
                    : sym + (p.currencySymbolPosition == 2 ? QStringLiteral(" ") : QString()) + body;
        return x < 0 ? QStringLiteral("(%1)").arg(s) : s;
    };
    if (lf == QLatin1String("general number")) return numeric ? fmtNum(n) : asStr(v);
    if (lf == QLatin1String("currency") && numeric) return currency(n);
    if (lf == QLatin1String("fixed") && numeric) return formatNumberPattern(n, QStringLiteral("0.00"));
    if (lf == QLatin1String("standard") && numeric) return formatNumberPattern(n, QStringLiteral("#,##0.00"));
    if (lf == QLatin1String("percent") && numeric) return formatNumberPattern(n, QStringLiteral("0.00%"));
    if (lf == QLatin1String("scientific") && numeric) return QString::number(n, 'E', 2);
    if (lf == QLatin1String("yes/no")) return truth(v) ? QStringLiteral("Yes") : QStringLiteral("No");
    if (lf == QLatin1String("true/false")) return truth(v) ? QStringLiteral("True") : QStringLiteral("False");
    if (lf == QLatin1String("on/off")) return truth(v) ? QStringLiteral("On") : QStringLiteral("Off");

    static const QHash<QString, QString> datePatterns = {
        { QStringLiteral("general date"), QString() },
        { QStringLiteral("long date"), QStringLiteral("dddd, mmmm d, yyyy") },
        { QStringLiteral("medium date"), QStringLiteral("dd-mmm-yy") },
        { QStringLiteral("short date"), QStringLiteral("m/d/yyyy") },
        { QStringLiteral("long time"), QStringLiteral("h:nn:ss AM/PM") },
        { QStringLiteral("medium time"), QStringLiteral("hh:nn AM/PM") },
        { QStringLiteral("short time"), QStringLiteral("hh:nn") },
    };
    QDateTime d;
    const auto named = datePatterns.constFind(lf);
    if (named != datePatterns.constEnd()) {
        if (!asDate(v, &d)) return asStr(v);
        return named->isEmpty() ? fmtDate(d) : formatDatePattern(d, *named);
    }
    static const QRegularExpression dateTokens(QStringLiteral("[ymdhns]"),
                                               QRegularExpression::CaseInsensitiveOption);
    if (v.k == Val::Date || (!numeric && dateTokens.match(f).hasMatch())) {
        if (asDate(v, &d)) return formatDatePattern(d, f);
    }
    if (numeric) return formatNumberPattern(n, f);
    return asStr(v);
}

// ---- parse tree --------------------------------------------------------------

struct Node {
    enum Type { Lit, Field, Neg, Not, Bin, Call };
    Type t = Lit;
    Val lit;
    quint32 fieldId = 0;           // Field: full id; 0 = a custom field known by name
    const FieldDef *builtin = nullptr;
    QString name;                  // Field name / Call function / Bin operator
    std::vector<Node *> kids;
};

class Parser {
public:
    Parser(const Project &p, const QString &text) : project(p), src(text.trimmed())
    {
        if (src.startsWith(QLatin1Char('='))) src.remove(0, 1);
    }

    Node *parse()
    {
        Node *root = expr();
        skip();
        if (!message.isEmpty()) return nullptr;
        if (pos < src.size())
            fail(QStringLiteral("Unexpected \"%1\" at position %2").arg(src.mid(pos, 12)).arg(pos + 1));
        return message.isEmpty() ? root : nullptr;
    }
    QString message;
    QList<quint32> references;       // custom fields the formula reads

private:
    Node *make(Node::Type t)
    {
        nodes.push_back(std::make_unique<Node>());
        nodes.back()->t = t;
        return nodes.back().get();
    }
    Node *binary(const QString &op, Node *a, Node *b)
    {
        Node *n = make(Node::Bin);
        n->name = op;
        n->kids = { a, b };
        return n;
    }
    void fail(const QString &m) { if (message.isEmpty()) message = m; }
    void skip() { while (pos < src.size() && src.at(pos).isSpace()) ++pos; }
    static bool identChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }
    bool op(const char *s)
    {
        skip();
        const QLatin1String l(s);
        if (src.mid(pos, l.size()) != l) return false;
        pos += int(l.size());
        return true;
    }
    bool word(const char *s)
    {
        skip();
        const QLatin1String l(s);
        if (src.mid(pos, l.size()).compare(l, Qt::CaseInsensitive) != 0) return false;
        if (pos + l.size() < src.size() && identChar(src.at(pos + l.size()))) return false;
        pos += int(l.size());
        return true;
    }

    // Lowest to highest: Imp, Eqv, Xor, Or, And, Not, comparison, &, + -, Mod,
    // \, * /, unary -, ^.
    Node *expr() { return imp(); }
    Node *imp() { Node *a = eqv(); while (message.isEmpty() && word("Imp")) a = binary("imp", a, eqv()); return a; }
    Node *eqv() { Node *a = xorOp(); while (message.isEmpty() && word("Eqv")) a = binary("eqv", a, xorOp()); return a; }
    Node *xorOp() { Node *a = orOp(); while (message.isEmpty() && word("Xor")) a = binary("xor", a, orOp()); return a; }
    Node *orOp() { Node *a = andOp(); while (message.isEmpty() && word("Or")) a = binary("or", a, andOp()); return a; }
    Node *andOp() { Node *a = notOp(); while (message.isEmpty() && word("And")) a = binary("and", a, notOp()); return a; }
    Node *notOp()
    {
        if (word("Not")) { Node *n = make(Node::Not); n->kids = { notOp() }; return n; }
        return compare();
    }
    Node *compare()
    {
        Node *a = concat();
        for (;;) {
            if (!message.isEmpty()) return a;
            if (op("<=")) a = binary("<=", a, concat());
            else if (op(">=")) a = binary(">=", a, concat());
            else if (op("<>")) a = binary("<>", a, concat());
            else if (op("=")) a = binary("=", a, concat());
            else if (op("<")) a = binary("<", a, concat());
            else if (op(">")) a = binary(">", a, concat());
            else if (word("Like")) a = binary("like", a, concat());
            else return a;
        }
    }
    Node *concat() { Node *a = add(); while (message.isEmpty() && op("&")) a = binary("&", a, add()); return a; }
    Node *add()
    {
        Node *a = mod();
        for (;;) {
            if (!message.isEmpty()) return a;
            if (op("+")) a = binary("+", a, mod());
            else if (op("-")) a = binary("-", a, mod());
            else return a;
        }
    }
    Node *mod() { Node *a = intDiv(); while (message.isEmpty() && word("Mod")) a = binary("mod", a, intDiv()); return a; }
    Node *intDiv() { Node *a = mul(); while (message.isEmpty() && op("\\")) a = binary("\\", a, mul()); return a; }
    Node *mul()
    {
        Node *a = unary();
        for (;;) {
            if (!message.isEmpty()) return a;
            if (op("*")) a = binary("*", a, unary());
            else if (op("/")) a = binary("/", a, unary());
            else return a;
        }
    }
    Node *unary()
    {
        if (op("-")) { Node *n = make(Node::Neg); n->kids = { unary() }; return n; }
        if (op("+")) return unary();
        return power();
    }
    Node *power()
    {
        Node *a = primary();
        while (message.isEmpty() && op("^")) {
            Node *b = nullptr;
            if (op("-")) { b = make(Node::Neg); b->kids = { primary() }; }
            else b = primary();
            a = binary("^", a, b);
        }
        return a;
    }

    Node *literal(const Val &v) { Node *n = make(Node::Lit); n->lit = v; return n; }

    Node *field(const QString &rawName)
    {
        const QString name = rawName.trimmed();
        Node *n = make(Node::Field);
        n->name = name;
        static const QRegularExpression mspj(QStringLiteral("^MSPJ(\\d+)$"),
                                             QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = mspj.match(name);
        if (m.hasMatch()) {
            const quint32 id = m.captured(1).toUInt();
            if (isTaskId(id) && (n->builtin = fieldByIndex(quint16(id & 0xFFFF)))) return n;
            if (slotDef(id)) {
                n->fieldId = id;
                const CustomField *d = definitionFor(project, id, QString());
                n->name = d ? d->name : QString::fromLatin1(slotDef(id)->name);
                references.append(id);
                return n;
            }
            fail(QStringLiteral("Unknown field [%1]").arg(name));
            return n;
        }
        if ((n->builtin = fieldByName(name))) return n;
        if (const CustomField *d = definitionFor(project, 0, name)) {
            n->fieldId = quint32(d->fieldId);
            if (n->fieldId) references.append(n->fieldId);
            return n;
        }
        if (const MppFieldIds::CustomFieldDef *s = slotByName(name)) {
            n->fieldId = taskId(s->index);
            references.append(n->fieldId);
            return n;
        }
        // A custom field only the tasks carry (an older file with no definitions).
        for (const Task &t : project.tasks)
            for (const CustomField &f : t.customFields)
                if (f.name.compare(name, Qt::CaseInsensitive) == 0) {
                    n->fieldId = quint32(f.fieldId);
                    return n;
                }
        fail(QStringLiteral("Unknown field [%1]").arg(name));
        return n;
    }

    Node *primary()
    {
        skip();
        if (pos >= src.size()) { fail(QStringLiteral("The formula ends too soon")); return literal(Val()); }
        const QChar c = src.at(pos);
        if (c == QLatin1Char('(')) {
            ++pos;
            Node *n = expr();
            if (!op(")")) fail(QStringLiteral("Missing )"));
            return n;
        }
        if (c == QLatin1Char('[')) {
            const int end = int(src.indexOf(QLatin1Char(']'), pos));
            if (end < 0) { fail(QStringLiteral("Missing ]")); return literal(Val()); }
            const QString name = src.mid(pos + 1, end - pos - 1);
            pos = end + 1;
            return field(name);
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            QString s;
            ++pos;
            for (;;) {
                if (pos >= src.size()) { fail(QStringLiteral("Unterminated string")); break; }
                if (src.at(pos) == c) {
                    if (pos + 1 < src.size() && src.at(pos + 1) == c) { s += c; pos += 2; continue; }
                    ++pos;
                    break;
                }
                s += src.at(pos++);
            }
            return literal(Val::str(s));
        }
        if (c == QLatin1Char('#')) {
            const int end = int(src.indexOf(QLatin1Char('#'), pos + 1));
            if (end < 0) { fail(QStringLiteral("Missing # after a date")); return literal(Val()); }
            const QDateTime d = parseDate(src.mid(pos + 1, end - pos - 1));
            if (!d.isValid()) fail(QStringLiteral("Not a date: #%1#").arg(src.mid(pos + 1, end - pos - 1)));
            pos = end + 1;
            return literal(Val::date(d));
        }
        if (c.isDigit() || c == QLatin1Char('.')) {
            const int begin = pos;
            while (pos < src.size() && (src.at(pos).isDigit() || src.at(pos) == QLatin1Char('.'))) ++pos;
            if (pos < src.size() && (src.at(pos) == QLatin1Char('e') || src.at(pos) == QLatin1Char('E'))) {
                int p = pos + 1;
                if (p < src.size() && (src.at(p) == QLatin1Char('+') || src.at(p) == QLatin1Char('-'))) ++p;
                if (p < src.size() && src.at(p).isDigit()) {
                    pos = p;
                    while (pos < src.size() && src.at(pos).isDigit()) ++pos;
                }
            }
            bool ok = false;
            const double v = src.mid(begin, pos - begin).toDouble(&ok);
            if (!ok) fail(QStringLiteral("Not a number: %1").arg(src.mid(begin, pos - begin)));
            return literal(Val::num(v));
        }
        if (c == QLatin1Char('&') && pos + 1 < src.size() && src.at(pos + 1).toUpper() == QLatin1Char('H')) {
            int begin = pos + 2;
            pos = begin;
            while (pos < src.size() && isxdigit(src.at(pos).toLatin1())) ++pos;
            return literal(Val::num(double(src.mid(begin, pos - begin).toLongLong(nullptr, 16))));
        }
        if (c.isLetter() || c == QLatin1Char('_')) {
            const int begin = pos;
            while (pos < src.size() && identChar(src.at(pos))) ++pos;
            const QString word = src.mid(begin, pos - begin);
            skip();
            if (pos < src.size() && src.at(pos) == QLatin1Char('(')) {
                ++pos;
                Node *n = make(Node::Call);
                n->name = word.toLower();
                if (!op(")")) {
                    do { n->kids.push_back(expr()); } while (message.isEmpty() && op(","));
                    if (!op(")")) fail(QStringLiteral("Missing ) after %1(").arg(word));
                }
                checkCall(n, word);
                return n;
            }
            const QString lw = word.toLower();
            if (lw == QLatin1String("true")) return literal(Val::boolean(true));
            if (lw == QLatin1String("false")) return literal(Val::boolean(false));
            if (lw == QLatin1String("null") || lw == QLatin1String("empty")) return literal(Val());
            static const QHash<QString, double> constants = {
                { QStringLiteral("pjminutes"), 3 }, { QStringLiteral("pjhours"), 5 },
                { QStringLiteral("pjdays"), 7 }, { QStringLiteral("pjweeks"), 9 },
                { QStringLiteral("pjmonths"), 11 }, { QStringLiteral("pjelapsedminutes"), 4 },
                { QStringLiteral("pjelapsedhours"), 6 }, { QStringLiteral("pjelapseddays"), 8 },
                { QStringLiteral("pjelapsedweeks"), 10 }, { QStringLiteral("pjelapsedmonths"), 12 },
                { QStringLiteral("vbsunday"), 1 }, { QStringLiteral("vbmonday"), 2 },
                { QStringLiteral("vbtuesday"), 3 }, { QStringLiteral("vbwednesday"), 4 },
                { QStringLiteral("vbthursday"), 5 }, { QStringLiteral("vbfriday"), 6 },
                { QStringLiteral("vbsaturday"), 7 }, { QStringLiteral("vbbinarycompare"), 0 },
                { QStringLiteral("vbtextcompare"), 1 },
            };
            const auto k = constants.constFind(lw);
            if (k != constants.constEnd()) return literal(Val::num(k.value()));
            fail(QStringLiteral("Unknown name %1 (put field names in [brackets])").arg(word));
            return literal(Val());
        }
        fail(QStringLiteral("Unexpected \"%1\" at position %2").arg(c).arg(pos + 1));
        ++pos;
        return literal(Val());
    }

    void checkCall(Node *n, const QString &word)
    {
        struct Arity { int min; int max; };
        static const QHash<QString, Arity> known = {
            { "iif", {3, 3} }, { "switch", {2, 64} }, { "choose", {2, 64} },
            { "abs", {1, 1} }, { "int", {1, 1} }, { "fix", {1, 1} }, { "round", {1, 2} },
            { "sqr", {1, 1} }, { "sgn", {1, 1} }, { "exp", {1, 1} }, { "log", {1, 1} },
            { "sin", {1, 1} }, { "cos", {1, 1} }, { "tan", {1, 1} }, { "atn", {1, 1} },
            { "len", {1, 1} }, { "left", {2, 2} }, { "right", {2, 2} }, { "mid", {2, 3} },
            { "ucase", {1, 1} }, { "lcase", {1, 1} }, { "trim", {1, 1} }, { "ltrim", {1, 1} },
            { "rtrim", {1, 1} }, { "instr", {2, 4} }, { "instrrev", {2, 4} }, { "replace", {3, 6} },
            { "space", {1, 1} }, { "string", {2, 2} }, { "strreverse", {1, 1} },
            { "strcomp", {2, 3} }, { "asc", {1, 1} }, { "chr", {1, 1} }, { "val", {1, 1} },
            { "isnumeric", {1, 1} }, { "isdate", {1, 1} }, { "isnull", {1, 1} },
            { "cstr", {1, 1} }, { "str", {1, 1} }, { "cdbl", {1, 1} }, { "csng", {1, 1} },
            { "ccur", {1, 1} }, { "cint", {1, 1} }, { "clng", {1, 1} }, { "cbool", {1, 1} },
            { "cdate", {1, 1} }, { "cvar", {1, 1} }, { "format", {1, 2} },
            { "now", {0, 0} }, { "date", {0, 0} }, { "time", {0, 0} },
            { "year", {1, 1} }, { "month", {1, 1} }, { "day", {1, 1} }, { "hour", {1, 1} },
            { "minute", {1, 1} }, { "second", {1, 1} }, { "weekday", {1, 2} },
            { "dateserial", {3, 3} }, { "timeserial", {3, 3} }, { "datevalue", {1, 1} },
            { "timevalue", {1, 1} }, { "dateadd", {3, 3} }, { "datediff", {3, 4} },
            { "datepart", {2, 3} }, { "monthname", {1, 2} }, { "weekdayname", {1, 3} },
            { "projdatediff", {2, 3} }, { "projdateadd", {2, 3} }, { "projdatesub", {2, 3} },
            { "projdatevalue", {1, 1} }, { "projdurvalue", {1, 1} }, { "projdurconv", {2, 2} },
        };
        const auto it = known.constFind(n->name);
        if (it == known.constEnd()) { fail(QStringLiteral("Unknown function %1()").arg(word)); return; }
        const int argc = int(n->kids.size());
        if (argc < it->min || argc > it->max)
            fail(QStringLiteral("%1() takes %2 argument%3").arg(word)
                     .arg(it->min == it->max ? QString::number(it->min)
                                             : QStringLiteral("%1 to %2").arg(it->min).arg(it->max))
                     .arg(it->max == 1 ? QString() : QStringLiteral("s")));
        if ((n->name == QLatin1String("switch")) && argc % 2 != 0)
            fail(QStringLiteral("Switch() takes condition, value pairs"));
    }

    const Project &project;
    QString src;
    int pos = 0;
    std::vector<std::unique_ptr<Node>> nodes;
public:
    // Keep the nodes alive with the Parser.
    std::vector<std::unique_ptr<Node>> &arena() { return nodes; }
};

// ---- evaluation ----------------------------------------------------------------

class Evaluator {
public:
    Evaluator(const Project &p, const Task &t) : project(p), task(t) {}

    Val eval(const Node *n)
    {
        switch (n->t) {
        case Node::Lit: return n->lit;
        case Node::Field:
            if (n->builtin) return n->builtin->get(project, task);
            return customValue(task, n->fieldId, n->name);
        case Node::Neg: {
            const Val a = eval(n->kids[0]);
            if (a.k == Val::Err) return a;
            double x = 0.0;
            if (!asNum(a, &x)) return mismatch();
            return Val::num(-x);
        }
        case Node::Not: {
            const Val a = eval(n->kids[0]);
            if (a.k == Val::Err) return a;
            if (a.k == Val::Bool) return Val::boolean(!a.b);
            double x = 0.0;
            if (!asNum(a, &x)) return mismatch();
            return Val::num(double(~qint64(bankers(x))));
        }
        case Node::Bin: return binary(n);
        case Node::Call: return call(n);
        }
        return Val();
    }

private:
    static Val mismatch() { return Val::err(QStringLiteral("Type mismatch")); }

    Val binary(const Node *n)
    {
        const QString &o = n->name;
        const Val a = eval(n->kids[0]);
        if (a.k == Val::Err) return a;
        const Val b = eval(n->kids[1]);
        if (b.k == Val::Err) return b;

        if (o == QLatin1String("&")) return Val::str(asStr(a) + asStr(b));
        if (o == QLatin1String("=") || o == QLatin1String("<>") || o == QLatin1String("<")
            || o == QLatin1String(">") || o == QLatin1String("<=") || o == QLatin1String(">=")) {
            const int c = compare(a, b);
            if (o == QLatin1String("=")) return Val::boolean(c == 0);
            if (o == QLatin1String("<>")) return Val::boolean(c != 0);
            if (o == QLatin1String("<")) return Val::boolean(c < 0);
            if (o == QLatin1String(">")) return Val::boolean(c > 0);
            if (o == QLatin1String("<=")) return Val::boolean(c <= 0);
            return Val::boolean(c >= 0);
        }
        if (o == QLatin1String("like")) return Val::boolean(like(asStr(a), asStr(b)));
        if (o == QLatin1String("and") || o == QLatin1String("or") || o == QLatin1String("xor")
            || o == QLatin1String("eqv") || o == QLatin1String("imp")) {
            if (a.k == Val::Bool && b.k == Val::Bool) {
                if (o == QLatin1String("and")) return Val::boolean(a.b && b.b);
                if (o == QLatin1String("or")) return Val::boolean(a.b || b.b);
                if (o == QLatin1String("xor")) return Val::boolean(a.b != b.b);
                if (o == QLatin1String("eqv")) return Val::boolean(a.b == b.b);
                return Val::boolean(!a.b || b.b);
            }
            double x = 0.0, y = 0.0;
            if (!asNum(a, &x) || !asNum(b, &y)) return mismatch();
            const qint64 p = qint64(bankers(x)), q = qint64(bankers(y));
            if (o == QLatin1String("and")) return Val::num(double(p & q));
            if (o == QLatin1String("or")) return Val::num(double(p | q));
            if (o == QLatin1String("xor")) return Val::num(double(p ^ q));
            if (o == QLatin1String("eqv")) return Val::num(double(~(p ^ q)));
            return Val::num(double(~p | q));
        }
        // Arithmetic. + joins two strings; dates move by days and differ in days.
        if (o == QLatin1String("+") && a.k == Val::Str && b.k == Val::Str) return Val::str(a.s + b.s);
        double x = 0.0, y = 0.0;
        if (!asNum(a, &x) || !asNum(b, &y)) return mismatch();
        if (o == QLatin1String("+")) {
            if ((a.k == Val::Date) != (b.k == Val::Date))
                return Val::date(fromOle(x + y));
            return Val::num(x + y);
        }
        if (o == QLatin1String("-")) {
            if (a.k == Val::Date && b.k != Val::Date) return Val::date(fromOle(x - y));
            return Val::num(x - y);
        }
        if (o == QLatin1String("*")) return Val::num(x * y);
        if (o == QLatin1String("/")) {
            if (y == 0.0) return Val::err(QStringLiteral("Division by zero"));
            return Val::num(x / y);
        }
        if (o == QLatin1String("\\") || o == QLatin1String("mod")) {
            const qint64 p = qint64(bankers(x)), q = qint64(bankers(y));
            if (q == 0) return Val::err(QStringLiteral("Division by zero"));
            return Val::num(o == QLatin1String("mod") ? double(p % q) : double(p / q));
        }
        if (o == QLatin1String("^")) return Val::num(std::pow(x, y));
        return Val::err(QStringLiteral("Unknown operator %1").arg(o));
    }

    static int compare(const Val &a, const Val &b)
    {
        double x = 0.0, y = 0.0;
        const bool numA = a.k != Val::Str || parseNumber(a.s, &x);
        const bool numB = b.k != Val::Str || parseNumber(b.s, &y);
        if (a.k == Val::Str && b.k == Val::Str)
            return QString::compare(a.s, b.s, Qt::CaseInsensitive);
        if (numA && numB && asNum(a, &x) && asNum(b, &y))
            return x < y ? -1 : x > y ? 1 : 0;
        return QString::compare(asStr(a), asStr(b), Qt::CaseInsensitive);
    }

    static bool like(const QString &text, const QString &pattern)
    {
        QString rx;
        for (int i = 0; i < pattern.size(); ++i) {
            const QChar c = pattern.at(i);
            if (c == QLatin1Char('*')) rx += QStringLiteral(".*");
            else if (c == QLatin1Char('?')) rx += QLatin1Char('.');
            else if (c == QLatin1Char('#')) rx += QStringLiteral("\\d");
            else if (c == QLatin1Char('[')) {
                const int end = int(pattern.indexOf(QLatin1Char(']'), i + 1));
                if (end < 0) { rx += QStringLiteral("\\["); continue; }
                QString set = pattern.mid(i + 1, end - i - 1);
                if (set.startsWith(QLatin1Char('!'))) set[0] = QLatin1Char('^');
                rx += QLatin1Char('[') + set + QLatin1Char(']');
                i = end;
            } else rx += QRegularExpression::escape(QString(c));
        }
        return QRegularExpression(QRegularExpression::anchoredPattern(rx),
                                  QRegularExpression::CaseInsensitiveOption).match(text).hasMatch();
    }

    WorkCalendar calendarArg(const std::vector<Val> &v, int index)
    {
        if (int(v.size()) > index && v[index].k == Val::Str) {
            for (const Calendar &c : project.calendars)
                if (c.name.compare(v[index].s, Qt::CaseInsensitive) == 0)
                    return WorkCalendar(project, c.uniqueId);
        }
        return WorkCalendar(project, project.calendarUniqueId);
    }

    double minutesPerUnit(int unit) const
    {
        const double perDay = project.minutesPerDay > 0 ? project.minutesPerDay : 480;
        const double perWeek = project.minutesPerWeek > 0 ? project.minutesPerWeek : 2400;
        const double perMonth = perDay * (project.daysPerMonth > 0 ? project.daysPerMonth : 20);
        switch (unit) {
        case 3: return 1;
        case 4: return 1;
        case 5: return 60;
        case 6: return 60;
        case 7: return perDay;
        case 8: return 1440;
        case 9: return perWeek;
        case 10: return 10080;
        case 11: return perMonth;
        case 12: return 43200;
        default: return perDay;
        }
    }

    Val call(const Node *n)
    {
        const QString &f = n->name;
        // Lazy: only the branch taken is evaluated, so IIf([Work]=0,0,[Cost]/[Work])
        // never divides by zero.
        if (f == QLatin1String("iif")) {
            const Val c = eval(n->kids[0]);
            if (c.k == Val::Err) return c;
            return eval(n->kids[truth(c) ? 1 : 2]);
        }
        if (f == QLatin1String("switch")) {
            for (size_t i = 0; i + 1 < n->kids.size(); i += 2) {
                const Val c = eval(n->kids[i]);
                if (c.k == Val::Err) return c;
                if (truth(c)) return eval(n->kids[i + 1]);
            }
            return Val();
        }
        if (f == QLatin1String("choose")) {
            const Val c = eval(n->kids[0]);
            if (c.k == Val::Err) return c;
            double i = 0.0;
            if (!asNum(c, &i)) return mismatch();
            const int k = int(std::floor(i));
            if (k < 1 || k >= int(n->kids.size())) return Val();
            return eval(n->kids[k]);
        }

        std::vector<Val> v;
        for (const Node *k : n->kids) {
            v.push_back(eval(k));
            if (v.back().k == Val::Err) return v.back();
        }
        auto numArg = [&](int i, double *out) { return asNum(v[i], out); };
        auto intArg = [&](int i, int fallback) {
            double x = fallback;
            if (int(v.size()) > i) asNum(v[i], &x);
            return int(bankers(x));
        };
        auto dateArg = [&](int i, QDateTime *out) { return asDate(v[i], out); };
        auto oneNum = [&](const std::function<double(double)> &fn) -> Val {
            double x = 0.0;
            if (!numArg(0, &x)) return mismatch();
            return Val::num(fn(x));
        };

        if (f == QLatin1String("abs")) return oneNum([](double x) { return std::fabs(x); });
        if (f == QLatin1String("int")) return oneNum([](double x) { return std::floor(x); });
        if (f == QLatin1String("fix")) return oneNum([](double x) { return std::trunc(x); });
        if (f == QLatin1String("sgn")) return oneNum([](double x) { return double((x > 0) - (x < 0)); });
        if (f == QLatin1String("exp")) return oneNum([](double x) { return std::exp(x); });
        if (f == QLatin1String("sin")) return oneNum([](double x) { return std::sin(x); });
        if (f == QLatin1String("cos")) return oneNum([](double x) { return std::cos(x); });
        if (f == QLatin1String("tan")) return oneNum([](double x) { return std::tan(x); });
        if (f == QLatin1String("atn")) return oneNum([](double x) { return std::atan(x); });
        if (f == QLatin1String("sqr")) {
            double x = 0.0;
            if (!numArg(0, &x)) return mismatch();
            if (x < 0) return Val::err(QStringLiteral("Sqr() of a negative number"));
            return Val::num(std::sqrt(x));
        }
        if (f == QLatin1String("log")) {
            double x = 0.0;
            if (!numArg(0, &x)) return mismatch();
            if (x <= 0) return Val::err(QStringLiteral("Log() of a number that is not positive"));
            return Val::num(std::log(x));
        }
        if (f == QLatin1String("round")) {
            double x = 0.0;
            if (!numArg(0, &x)) return mismatch();
            const double scale = std::pow(10.0, intArg(1, 0));
            return Val::num(bankers(x * scale) / scale);
        }
        if (f == QLatin1String("len")) return Val::num(asStr(v[0]).size());
        if (f == QLatin1String("left")) return Val::str(asStr(v[0]).left(qMax(0, intArg(1, 0))));
        if (f == QLatin1String("right")) return Val::str(asStr(v[0]).right(qMax(0, intArg(1, 0))));
        if (f == QLatin1String("mid")) {
            const int start = intArg(1, 1);
            if (start < 1) return Val::err(QStringLiteral("Mid() start must be 1 or more"));
            return Val::str(v.size() > 2 ? asStr(v[0]).mid(start - 1, qMax(0, intArg(2, 0)))
                                         : asStr(v[0]).mid(start - 1));
        }
        if (f == QLatin1String("ucase")) return Val::str(asStr(v[0]).toUpper());
        if (f == QLatin1String("lcase")) return Val::str(asStr(v[0]).toLower());
        if (f == QLatin1String("trim")) return Val::str(asStr(v[0]).trimmed());
        if (f == QLatin1String("ltrim")) {
            QString s = asStr(v[0]);
            while (!s.isEmpty() && s.at(0) == QLatin1Char(' ')) s.remove(0, 1);
            return Val::str(s);
        }
        if (f == QLatin1String("rtrim")) {
            QString s = asStr(v[0]);
            while (s.endsWith(QLatin1Char(' '))) s.chop(1);
            return Val::str(s);
        }
        if (f == QLatin1String("instr") || f == QLatin1String("instrrev")) {
            // InStr([start,] string1, string2[, compare]); InStrRev(s1, s2[, start[, compare]])
            const bool rev = f == QLatin1String("instrrev");
            int start = rev ? -1 : 1;
            int si = 0;
            if (!rev && v.size() >= 3 && v[0].k != Val::Str) { start = intArg(0, 1); si = 1; }
            const QString hay = asStr(v[si]), needle = asStr(v[si + 1]);
            int cmpIndex = rev ? 3 : si + 2;
            if (rev && v.size() > 2) start = intArg(2, -1);
            const Qt::CaseSensitivity cs = int(v.size()) > cmpIndex && intArg(cmpIndex, 0) == 1
                                           ? Qt::CaseInsensitive : Qt::CaseSensitive;
            if (rev) {
                const int from = start < 0 ? -1 : start - 1;
                return Val::num(double(hay.lastIndexOf(needle, from, cs) + 1));
            }
            if (start < 1) return Val::err(QStringLiteral("InStr() start must be 1 or more"));
            return Val::num(double(hay.indexOf(needle, start - 1, cs) + 1));
        }
        if (f == QLatin1String("replace")) {
            QString s = asStr(v[0]);
            return Val::str(s.replace(asStr(v[1]), asStr(v[2]), Qt::CaseSensitive));
        }
        if (f == QLatin1String("space")) return Val::str(QString(qMax(0, intArg(0, 0)), QLatin1Char(' ')));
        if (f == QLatin1String("string")) {
            const QString c = v[1].k == Val::Str ? v[1].s.left(1) : QString(QChar(intArg(1, 32)));
            return Val::str(c.repeated(qMax(0, intArg(0, 0))));
        }
        if (f == QLatin1String("strreverse")) {
            QString s = asStr(v[0]);
            std::reverse(s.begin(), s.end());
            return Val::str(s);
        }
        if (f == QLatin1String("strcomp")) {
            const Qt::CaseSensitivity cs = v.size() > 2 && intArg(2, 0) == 1 ? Qt::CaseInsensitive
                                                                            : Qt::CaseSensitive;
            const int c = QString::compare(asStr(v[0]), asStr(v[1]), cs);
            return Val::num(double((c > 0) - (c < 0)));
        }
        if (f == QLatin1String("asc")) {
            const QString s = asStr(v[0]);
            if (s.isEmpty()) return Val::err(QStringLiteral("Asc() of an empty string"));
            return Val::num(s.at(0).unicode());
        }
        if (f == QLatin1String("chr")) return Val::str(QString(QChar(intArg(0, 32))));
        if (f == QLatin1String("val")) {
            static const QRegularExpression lead(QStringLiteral("^\\s*[-+]?(\\d+\\.?\\d*|\\.\\d+)([eE][-+]?\\d+)?"));
            const QRegularExpressionMatch m = lead.match(asStr(v[0]));
            return Val::num(m.hasMatch() ? m.captured(0).trimmed().toDouble() : 0.0);
        }
        if (f == QLatin1String("isnumeric")) {
            double x = 0.0;
            return Val::boolean(v[0].k != Val::Date && v[0].k != Val::Empty && asNum(v[0], &x));
        }
        if (f == QLatin1String("isdate")) {
            return Val::boolean(v[0].k == Val::Date || (v[0].k == Val::Str && parseDate(v[0].s).isValid()));
        }
        if (f == QLatin1String("isnull")) return Val::boolean(v[0].k == Val::Empty);
        if (f == QLatin1String("cstr") || f == QLatin1String("cvar")) return f == QLatin1String("cvar") ? v[0] : Val::str(asStr(v[0]));
        if (f == QLatin1String("str")) {
            double x = 0.0;
            if (!numArg(0, &x)) return mismatch();
            return Val::str(x >= 0 ? QLatin1Char(' ') + fmtNum(x) : fmtNum(x));
        }
        if (f == QLatin1String("cdbl") || f == QLatin1String("csng") || f == QLatin1String("ccur")) {
            double x = 0.0;
            if (!numArg(0, &x)) return mismatch();
            return Val::num(x);
        }
        if (f == QLatin1String("cint") || f == QLatin1String("clng")) {
            double x = 0.0;
            if (!numArg(0, &x)) return mismatch();
            return Val::num(bankers(x));
        }
        if (f == QLatin1String("cbool")) return Val::boolean(truth(v[0]));
        if (f == QLatin1String("cdate") || f == QLatin1String("datevalue")) {
            QDateTime d;
            if (!dateArg(0, &d)) return mismatch();
            if (f == QLatin1String("datevalue") && d.isValid()) d.setTime(QTime(0, 0));
            return Val::date(d);
        }
        if (f == QLatin1String("timevalue")) {
            QDateTime d;
            if (!dateArg(0, &d) || !d.isValid()) return mismatch();
            return Val::date(QDateTime(QDate(1899, 12, 30), d.time()));
        }
        if (f == QLatin1String("format"))
            return Val::str(formatValue(project, v[0], v.size() > 1 ? asStr(v[1]) : QString()));
        if (f == QLatin1String("now")) {
            const QDateTime now = QDateTime::currentDateTime();
            return Val::date(QDateTime(now.date(), QTime(now.time().hour(), now.time().minute(), now.time().second())));
        }
        if (f == QLatin1String("date")) return Val::date(QDateTime(QDate::currentDate(), QTime(0, 0)));
        if (f == QLatin1String("time")) return Val::date(QDateTime(QDate(1899, 12, 30), QTime::currentTime()));
        if (f == QLatin1String("year") || f == QLatin1String("month") || f == QLatin1String("day")
            || f == QLatin1String("hour") || f == QLatin1String("minute") || f == QLatin1String("second")
            || f == QLatin1String("weekday")) {
            QDateTime d;
            if (!dateArg(0, &d)) return mismatch();
            if (!d.isValid()) return Val::err(QStringLiteral("NA is not a date"));
            if (f == QLatin1String("year")) return Val::num(d.date().year());
            if (f == QLatin1String("month")) return Val::num(d.date().month());
            if (f == QLatin1String("day")) return Val::num(d.date().day());
            if (f == QLatin1String("hour")) return Val::num(d.time().hour());
            if (f == QLatin1String("minute")) return Val::num(d.time().minute());
            if (f == QLatin1String("second")) return Val::num(d.time().second());
            const int first = intArg(1, 1);              // 1 = Sunday
            const int sundayBased = d.date().dayOfWeek() % 7 + 1;   // Sunday = 1
            return Val::num(double((sundayBased - first + 7) % 7 + 1));
        }
        if (f == QLatin1String("dateserial")) {
            const QDate d = QDate(intArg(0, 1900), 1, 1).addMonths(intArg(1, 1) - 1).addDays(intArg(2, 1) - 1);
            return Val::date(QDateTime(d, QTime(0, 0)));
        }
        if (f == QLatin1String("timeserial")) {
            const int secs = intArg(0, 0) * 3600 + intArg(1, 0) * 60 + intArg(2, 0);
            return Val::date(QDateTime(QDate(1899, 12, 30), QTime(0, 0)).addSecs(secs));
        }
        if (f == QLatin1String("monthname")) {
            const int m = intArg(0, 1);
            if (m < 1 || m > 12) return Val::err(QStringLiteral("MonthName() takes 1 to 12"));
            return Val::str(QLocale::c().monthName(m, v.size() > 1 && truth(v[1])
                                                         ? QLocale::ShortFormat : QLocale::LongFormat));
        }
        if (f == QLatin1String("weekdayname")) {
            const int w = intArg(0, 1);
            if (w < 1 || w > 7) return Val::err(QStringLiteral("WeekdayName() takes 1 to 7"));
            const int first = v.size() > 2 ? intArg(2, 1) : 1;
            const int sundayBased = (w - 1 + first - 1) % 7 + 1;
            const int qtDay = sundayBased == 1 ? 7 : sundayBased - 1;
            return Val::str(QLocale::c().dayName(qtDay, v.size() > 1 && truth(v[1])
                                                         ? QLocale::ShortFormat : QLocale::LongFormat));
        }
        if (f == QLatin1String("dateadd")) {
            QDateTime d;
            double amount = 0.0;
            if (!numArg(1, &amount) || !dateArg(2, &d)) return mismatch();
            if (!d.isValid()) return Val::err(QStringLiteral("NA is not a date"));
            const QString unit = asStr(v[0]).toLower();
            const int k = int(std::trunc(amount));
            if (unit == QLatin1String("yyyy")) return Val::date(d.addYears(k));
            if (unit == QLatin1String("q")) return Val::date(d.addMonths(3 * k));
            if (unit == QLatin1String("m")) return Val::date(d.addMonths(k));
            if (unit == QLatin1String("y") || unit == QLatin1String("d") || unit == QLatin1String("w"))
                return Val::date(d.addDays(k));
            if (unit == QLatin1String("ww")) return Val::date(d.addDays(7 * k));
            if (unit == QLatin1String("h")) return Val::date(d.addSecs(qint64(k) * 3600));
            if (unit == QLatin1String("n")) return Val::date(d.addSecs(qint64(k) * 60));
            if (unit == QLatin1String("s")) return Val::date(d.addSecs(k));
            return Val::err(QStringLiteral("Unknown DateAdd() interval \"%1\"").arg(unit));
        }
        if (f == QLatin1String("datediff") || f == QLatin1String("datepart")) {
            const bool diff = f == QLatin1String("datediff");
            QDateTime a, b;
            if (!dateArg(1, &a) || (diff && !dateArg(2, &b))) return mismatch();
            if (!a.isValid() || (diff && !b.isValid())) return Val::err(QStringLiteral("NA is not a date"));
            const QString unit = asStr(v[0]).toLower();
            const int first = diff ? (v.size() > 3 ? intArg(3, 1) : 1) : (v.size() > 2 ? intArg(2, 1) : 1);
            auto weekStart = [first](const QDate &d) {
                const int sundayBased = d.dayOfWeek() % 7 + 1;
                return d.addDays(-((sundayBased - first + 7) % 7));
            };
            if (!diff) {
                const QDate d = a.date();
                if (unit == QLatin1String("yyyy")) return Val::num(d.year());
                if (unit == QLatin1String("q")) return Val::num((d.month() - 1) / 3 + 1);
                if (unit == QLatin1String("m")) return Val::num(d.month());
                if (unit == QLatin1String("y")) return Val::num(d.dayOfYear());
                if (unit == QLatin1String("d")) return Val::num(d.day());
                if (unit == QLatin1String("w")) return Val::num((d.dayOfWeek() % 7 + 1 - first + 7) % 7 + 1);
                if (unit == QLatin1String("ww"))
                    return Val::num(weekStart(QDate(d.year(), 1, 1)).daysTo(weekStart(d)) / 7 + 1);
                if (unit == QLatin1String("h")) return Val::num(a.time().hour());
                if (unit == QLatin1String("n")) return Val::num(a.time().minute());
                if (unit == QLatin1String("s")) return Val::num(a.time().second());
                return Val::err(QStringLiteral("Unknown DatePart() interval \"%1\"").arg(unit));
            }
            const QDate da = a.date(), db = b.date();
            if (unit == QLatin1String("yyyy")) return Val::num(db.year() - da.year());
            if (unit == QLatin1String("q"))
                return Val::num((db.year() * 4 + (db.month() - 1) / 3) - (da.year() * 4 + (da.month() - 1) / 3));
            if (unit == QLatin1String("m")) return Val::num((db.year() * 12 + db.month()) - (da.year() * 12 + da.month()));
            if (unit == QLatin1String("y") || unit == QLatin1String("d")) return Val::num(double(da.daysTo(db)));
            if (unit == QLatin1String("w")) return Val::num(double(da.daysTo(db) / 7));
            if (unit == QLatin1String("ww")) return Val::num(double(weekStart(da).daysTo(weekStart(db)) / 7));
            const qint64 sa = wallSecs(a), sb = wallSecs(b);
            auto floorDiv = [](qint64 x, qint64 d) { return x >= 0 ? x / d : -((-x + d - 1) / d); };
            if (unit == QLatin1String("h")) return Val::num(double(floorDiv(sb, 3600) - floorDiv(sa, 3600)));
            if (unit == QLatin1String("n")) return Val::num(double(floorDiv(sb, 60) - floorDiv(sa, 60)));
            if (unit == QLatin1String("s")) return Val::num(double(sb - sa));
            return Val::err(QStringLiteral("Unknown DateDiff() interval \"%1\"").arg(unit));
        }
        if (f == QLatin1String("projdatediff")) {
            QDateTime a, b;
            if (!dateArg(0, &a) || !dateArg(1, &b)) return mismatch();
            if (!a.isValid() || !b.isValid()) return Val::err(QStringLiteral("NA is not a date"));
            const WorkCalendar cal = calendarArg(v, 2);
            const double m = a <= b ? minutes(cal.workBetween(a, b)) : -minutes(cal.workBetween(b, a));
            return Val::num(m);
        }
        if (f == QLatin1String("projdateadd") || f == QLatin1String("projdatesub")) {
            QDateTime d;
            double m = 0.0;
            if (!dateArg(0, &d)) return mismatch();
            if (!d.isValid()) return Val::err(QStringLiteral("NA is not a date"));
            if (v[1].k == Val::Str && !parseNumber(v[1].s, &m)) m = durationMinutes(v[1].s);
            else if (!numArg(1, &m)) return mismatch();
            if (f == QLatin1String("projdatesub")) m = -m;
            return Val::date(calendarArg(v, 2).addWork(d, qint64(std::llround(m * 60000.0))));
        }
        if (f == QLatin1String("projdatevalue")) {
            QDateTime d;
            if (!dateArg(0, &d)) return mismatch();
            return Val::date(d);
        }
        if (f == QLatin1String("projdurvalue")) {
            double m = 0.0;
            if (v[0].k != Val::Str || parseNumber(v[0].s, &m)) {
                if (!numArg(0, &m)) return mismatch();
                return Val::num(m);
            }
            return Val::num(durationMinutes(v[0].s));
        }
        if (f == QLatin1String("projdurconv")) {
            double m = 0.0;
            if (!numArg(0, &m)) return mismatch();
            const int unit = intArg(1, 7);
            static const QHash<int, QPair<QString, QString>> labels = {
                { 3, { QStringLiteral("min"), QStringLiteral("mins") } },
                { 4, { QStringLiteral("emin"), QStringLiteral("emins") } },
                { 5, { QStringLiteral("hr"), QStringLiteral("hrs") } },
                { 6, { QStringLiteral("ehr"), QStringLiteral("ehrs") } },
                { 7, { QStringLiteral("day"), QStringLiteral("days") } },
                { 8, { QStringLiteral("eday"), QStringLiteral("edays") } },
                { 9, { QStringLiteral("wk"), QStringLiteral("wks") } },
                { 10, { QStringLiteral("ewk"), QStringLiteral("ewks") } },
                { 11, { QStringLiteral("mon"), QStringLiteral("mons") } },
                { 12, { QStringLiteral("emon"), QStringLiteral("emons") } },
            };
            const double amount = std::round(m / minutesPerUnit(unit) * 100.0) / 100.0;
            const auto l = labels.value(unit, labels.value(7));
            return Val::str(fmtNum(amount) + QLatin1Char(' ') + (amount == 1.0 ? l.first : l.second));
        }
        return Val::err(QStringLiteral("Unknown function %1()").arg(f));
    }

    // "2d", "3 wks", "1.5h" -> minutes, by the project's working-time settings.
    double durationMinutes(const QString &text) const
    {
        static const QRegularExpression re(QStringLiteral("^\\s*([-+]?\\d*\\.?\\d+)\\s*(e?)([a-z]*)\\s*$"),
                                           QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = re.match(text);
        if (!m.hasMatch()) return 0.0;
        const double amount = m.captured(1).toDouble();
        const bool elapsed = !m.captured(2).isEmpty();
        const QString u = m.captured(3).toLower();
        int unit = 7;
        if (u.startsWith(QLatin1String("mi")) || u == QLatin1String("m")) unit = 3;
        else if (u.startsWith(QLatin1String("h"))) unit = 5;
        else if (u.startsWith(QLatin1String("d")) || u.isEmpty()) unit = 7;
        else if (u.startsWith(QLatin1String("w"))) unit = 9;
        else if (u.startsWith(QLatin1String("mo"))) unit = 11;
        return amount * minutesPerUnit(elapsed ? unit + 1 : unit);
    }

    const Project &project;
    const Task &task;
};

// Coerce a result to the custom field's kind and store it the way the MPP
// reader would have produced it.
QVariant toFieldValue(const Val &v, MppFieldIds::FieldKind kind, bool natural)
{
    using MppFieldIds::FieldKind;
    if (v.k == Val::Err) return QVariant();
    if (natural) {
        // A field with no Project slot keeps the result's own type.
        switch (v.k) {
        case Val::Num: return v.n;
        case Val::Bool: return v.b;
        case Val::Date: return v.d.isValid() ? QVariant(v.d) : QVariant();
        default: return asStr(v);
        }
    }
    double n = 0.0;
    switch (kind) {
    case FieldKind::String:
        return asStr(v);
    case FieldKind::Number:
    case FieldKind::Currency:
        if (!asNum(v, &n)) return QVariant();
        return n;
    case FieldKind::Bool:
        return truth(v);
    case FieldKind::DateTime: {
        QDateTime d;
        if (!asDate(v, &d) || !d.isValid()) return QVariant();
        return d;
    }
    case FieldKind::Duration:
        if (!asNum(v, &n)) return QVariant();
        return QVariant::fromValue<qint64>(qint64(std::llround(n * 60000.0)));
    }
    return QVariant();
}

struct Compiled {
    Parser parser;
    Node *root = nullptr;
    Compiled(const Project &p, const QString &text) : parser(p, text) { root = parser.parse(); }
};

// Project's indicator tests (PjComparison): numbers and dates compare as
// numbers, everything else as text; "any" matches every value.
bool compareRule(const QVariant &left, const CustomField::IndicatorRule &rule)
{
    const QString op = rule.comparison.toLower();
    const QString l = left.typeId() == QMetaType::Bool ? (left.toBool() ? QStringLiteral("Yes") : QStringLiteral("No"))
                                                       : left.toString();
    const QString r = rule.value.toString();
    if (op == QLatin1String("any")) return true;
    if (op == QLatin1String("contains")) return l.contains(r, Qt::CaseInsensitive);
    if (op == QLatin1String("notcontains")) return !l.contains(r, Qt::CaseInsensitive);
    if (op == QLatin1String("containsexactly")) return l == r;
    int cmp = 0;
    double a = 0.0, b = 0.0;
    QDateTime da = left.toDateTime();
    const QDateTime db = parseDate(r);
    if (left.typeId() == QMetaType::QDateTime && da.isValid() && db.isValid()) {
        cmp = da < db ? -1 : da > db ? 1 : 0;
    } else if (parseNumber(l, &a) && parseNumber(r, &b)) {
        cmp = a < b ? -1 : a > b ? 1 : 0;
    } else {
        cmp = QString::compare(l, r, Qt::CaseInsensitive);
    }
    if (op == QLatin1String("eq")) return cmp == 0;
    if (op == QLatin1String("ne")) return cmp != 0;
    if (op == QLatin1String("lt")) return cmp < 0;
    if (op == QLatin1String("le")) return cmp <= 0;
    if (op == QLatin1String("gt")) return cmp > 0;
    if (op == QLatin1String("ge")) return cmp >= 0;
    return false;
}

// Replace each [field] outside string literals by fn(name); fn returns the
// replacement text without brackets, or a null QString to keep it as written.
QString mapFieldRefs(const QString &formula, const std::function<QString(const QString &)> &fn)
{
    QString out;
    out.reserve(formula.size());
    for (int i = 0; i < formula.size(); ++i) {
        const QChar c = formula.at(i);
        if (c == QLatin1Char('"')) {
            const int end = int(formula.indexOf(QLatin1Char('"'), i + 1));
            const int stop = end < 0 ? int(formula.size()) : end + 1;
            out += formula.mid(i, stop - i);
            i = stop - 1;
            // A doubled quote inside a string reopens it; the loop sees it next.
        } else if (c == QLatin1Char('[')) {
            const int end = int(formula.indexOf(QLatin1Char(']'), i + 1));
            if (end < 0) { out += formula.mid(i); break; }
            const QString name = formula.mid(i + 1, end - i - 1);
            const QString mapped = fn(name);
            out += QLatin1Char('[') + (mapped.isNull() ? name : mapped) + QLatin1Char(']');
            i = end;
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

QVariant CustomFieldLogic::evaluate(const Project &project, const Task &task,
                                    const QString &formula, QString *error)
{
    Compiled c(project, formula);
    if (!c.root) {
        if (error) *error = c.parser.message;
        return QVariant();
    }
    const Val v = Evaluator(project, task).eval(c.root);
    if (v.k == Val::Err) {
        if (error) *error = v.s;
        return QVariant();
    }
    if (error) error->clear();
    switch (v.k) {
    case Val::Num: return v.n;
    case Val::Str: return v.s;
    case Val::Bool: return v.b;
    case Val::Date: return v.d.isValid() ? QVariant(v.d) : QVariant(QStringLiteral("NA"));
    default: return QVariant(QString());
    }
}

QString CustomFieldLogic::validate(const Project &project, const QString &formula)
{
    if (formula.trimmed().isEmpty()) return QString();
    Compiled c(project, formula);
    return c.root ? QString() : c.parser.message;
}

void CustomFieldLogic::recalculate(Project &project)
{
    // Older documents kept the formula only on each task's copy of the field.
    for (const Task &t : std::as_const(project.tasks))
        for (const CustomField &f : t.customFields)
            if (!f.formula.trimmed().isEmpty()
                && !definitionFor(project, quint32(f.fieldId), f.fieldId ? QString() : f.name)) {
                CustomField d = f;
                d.value = QVariant();
                project.customFieldDefinitions.append(d);
            }

    struct Item { const CustomField *def; std::unique_ptr<Compiled> code; MppFieldIds::FieldKind kind; };
    std::vector<Item> items;
    for (const CustomField &d : std::as_const(project.customFieldDefinitions)) {
        if (d.formula.trimmed().isEmpty()) continue;
        items.push_back({ &d, std::make_unique<Compiled>(project, d.formula),
                          d.fieldId ? kindOf(quint32(d.fieldId)) : MppFieldIds::FieldKind::String });
    }

    auto findField = [](Task &t, const CustomField &d) -> CustomField * {
        for (CustomField &f : t.customFields)
            if (d.fieldId ? f.fieldId == d.fieldId : f.name.compare(d.name, Qt::CaseInsensitive) == 0)
                return &f;
        return nullptr;
    };
    // Formula-less definitions: their task copies lose any formula they had.
    for (Task &t : project.tasks)
        for (const CustomField &d : std::as_const(project.customFieldDefinitions))
            if (d.formula.trimmed().isEmpty())
                if (CustomField *f = findField(t, d)) f->formula.clear();
    if (items.empty()) return;

    // Evaluate a field after the formula fields it reads (one pass suffices
    // unless the formulas refer to each other in a cycle).
    std::vector<int> order;
    std::vector<int> state(items.size(), 0);
    std::function<void(int)> visit = [&](int i) {
        if (state[i]) return;
        state[i] = 1;
        if (items[i].code->root)
            for (quint32 ref : items[i].code->parser.references)
                for (int j = 0; j < int(items.size()); ++j)
                    if (quint32(items[j].def->fieldId) == ref && j != i) visit(j);
        state[i] = 2;
        order.push_back(i);
    };
    for (int i = 0; i < int(items.size()); ++i) visit(i);

    for (Task &t : project.tasks) {
        for (const Item &item : items) {
            CustomField *f = findField(t, *item.def);
            if (!f) {
                CustomField copy = *item.def;
                copy.value = QVariant();
                t.customFields.append(copy);
                f = &t.customFields.last();
            }
            f->name = item.def->name;
            f->formula = item.def->formula;
            f->lookupValues = item.def->lookupValues;
            f->graphicalIndicators = item.def->graphicalIndicators;
            if (t.summary) f->value = QVariant();
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (Task &t : project.tasks) {
            if (t.summary) continue;
            for (int i : order) {
                const Item &item = items[i];
                CustomField *f = findField(t, *item.def);
                if (!f) continue;
                f->value = item.code->root
                    ? toFieldValue(Evaluator(project, t).eval(item.code->root), item.kind,
                                   item.def->fieldId == 0)
                    : QVariant();
            }
        }
    }
}

QString CustomFieldLogic::toProjectFormula(const Project &project, const QString &formula)
{
    return mapFieldRefs(formula, [&](const QString &name) {
        const quint32 id = taskFieldId(project, name);
        return id ? QStringLiteral("MSPJ%1").arg(id) : QString();
    });
}

QString CustomFieldLogic::fromProjectFormula(const Project &project, const QString &formula)
{
    static const QRegularExpression mspj(QStringLiteral("^MSPJ(\\d+)$"),
                                         QRegularExpression::CaseInsensitiveOption);
    return mapFieldRefs(formula, [&](const QString &name) -> QString {
        const QRegularExpressionMatch m = mspj.match(name.trimmed());
        if (!m.hasMatch()) return QString();
        const quint32 id = m.captured(1).toUInt();
        if (!isTaskId(id)) return QString();
        if (const FieldDef *f = fieldByIndex(quint16(id & 0xFFFF))) return QLatin1String(f->name);
        if (const CustomField *d = definitionFor(project, id, QString()))
            if (!d->name.isEmpty()) return d->name;
        const QString slot = slotName(id);
        return slot.isEmpty() ? QString() : slot;
    });
}

quint32 CustomFieldLogic::taskFieldId(const Project &project, const QString &rawName)
{
    const QString name = rawName.trimmed();
    static const QRegularExpression mspj(QStringLiteral("^MSPJ(\\d+)$"),
                                         QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = mspj.match(name);
    if (m.hasMatch()) return m.captured(1).toUInt();
    if (const FieldDef *f = fieldByName(name)) return taskId(f->index);
    if (const CustomField *d = definitionFor(project, 0, name))
        if (d->fieldId) return quint32(d->fieldId);
    if (const MppFieldIds::CustomFieldDef *s = slotByName(name)) return taskId(s->index);
    for (const Task &t : project.tasks)
        for (const CustomField &f : t.customFields)
            if (f.fieldId && f.name.compare(name, Qt::CaseInsensitive) == 0)
                return quint32(f.fieldId);
    return 0;
}

QString CustomFieldLogic::slotName(quint32 fieldId)
{
    const MppFieldIds::CustomFieldDef *d = slotDef(fieldId);
    return d ? QString::fromLatin1(d->name) : QString();
}

QStringList CustomFieldLogic::taskFieldNames()
{
    QStringList names;
    for (const FieldDef &f : fields()) names.append(QLatin1String(f.name));
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool CustomFieldLogic::acceptsLookupValue(const CustomField &field, const QVariant &value)
{ return field.lookupValues.isEmpty() || field.lookupValues.contains(value.toString(), Qt::CaseInsensitive); }

QString CustomFieldLogic::indicatorFor(const CustomField &field)
{ for (const auto &rule : field.graphicalIndicators) if (compareRule(field.value,rule)) return rule.indicator; return {}; }
}
