// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/duration.h"

#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <cmath>

namespace schedule {
namespace Duration {

bool isElapsed(int unit)
{
    switch (unit) {
    case ElapsedMinutes:
    case ElapsedHours:
    case ElapsedDays:
    case ElapsedWeeks:
    case ElapsedMonths:
        return true;
    default:
        return false;
    }
}

qint64 unitMillis(int unit)
{
    switch (unit) {
    case Minutes:        return kMillisPerMinute;
    case ElapsedMinutes: return kMillisPerMinute;
    case Hours:          return kMillisPerHour;
    case ElapsedHours:   return kMillisPerHour;
    case Days:           return kMillisPerDay;
    case ElapsedDays:    return kMillisPerElapsedDay;
    case Weeks:          return kMillisPerWeek;
    case ElapsedWeeks:   return kMillisPerElapsedWeek;
    case Months:         return kMillisPerMonth;
    case ElapsedMonths:  return kMillisPerElapsedMonth;
    default:             return kMillisPerDay;
    }
}

int normalizeUnit(int unit)
{
    if (unit >= Minutes && unit <= ElapsedMonths)
        return unit;
    if (unit >= Minutes + 32 && unit <= ElapsedMonths + 32)   // "estimated" variants
        return unit - 32;
    return Days;
}

qint64 toMillis(double value, int unit)
{
    return qint64(std::llround(value * double(unitMillis(unit))));
}

double fromMillis(qint64 millis, int unit)
{
    return double(millis) / double(unitMillis(unit));
}

namespace {

// Column abbreviations, singular/plural, in MS Project's English style.
const char *abbreviation(int unit, bool plural)
{
    switch (unit) {
    case Minutes:        return plural ? "mins" : "min";
    case ElapsedMinutes: return plural ? "emins" : "emin";
    case Hours:          return plural ? "hrs" : "hr";
    case ElapsedHours:   return plural ? "ehrs" : "ehr";
    case Days:           return plural ? "days" : "day";
    case ElapsedDays:    return plural ? "edays" : "eday";
    case Weeks:          return plural ? "wks" : "wk";
    case ElapsedWeeks:   return plural ? "ewks" : "ewk";
    case Months:         return plural ? "mons" : "mon";
    case ElapsedMonths:  return plural ? "emons" : "emon";
    default:             return plural ? "days" : "day";
    }
}

// Every spelling MS Project's duration edit accepts, lower-cased.
const QHash<QString, int> &suffixMap()
{
    static const QHash<QString, int> map = {
        { QStringLiteral("m"), Minutes },        { QStringLiteral("min"), Minutes },
        { QStringLiteral("mins"), Minutes },     { QStringLiteral("minute"), Minutes },
        { QStringLiteral("minutes"), Minutes },
        { QStringLiteral("h"), Hours },          { QStringLiteral("hr"), Hours },
        { QStringLiteral("hrs"), Hours },        { QStringLiteral("hour"), Hours },
        { QStringLiteral("hours"), Hours },
        { QStringLiteral("d"), Days },           { QStringLiteral("dy"), Days },
        { QStringLiteral("dys"), Days },         { QStringLiteral("day"), Days },
        { QStringLiteral("days"), Days },
        { QStringLiteral("w"), Weeks },          { QStringLiteral("wk"), Weeks },
        { QStringLiteral("wks"), Weeks },        { QStringLiteral("week"), Weeks },
        { QStringLiteral("weeks"), Weeks },
        { QStringLiteral("mo"), Months },        { QStringLiteral("mon"), Months },
        { QStringLiteral("mons"), Months },      { QStringLiteral("month"), Months },
        { QStringLiteral("months"), Months },
        { QStringLiteral("em"), ElapsedMinutes },  { QStringLiteral("emin"), ElapsedMinutes },
        { QStringLiteral("emins"), ElapsedMinutes },
        { QStringLiteral("eminute"), ElapsedMinutes },
        { QStringLiteral("eminutes"), ElapsedMinutes },
        { QStringLiteral("eh"), ElapsedHours },  { QStringLiteral("ehr"), ElapsedHours },
        { QStringLiteral("ehrs"), ElapsedHours },
        { QStringLiteral("ehour"), ElapsedHours },
        { QStringLiteral("ehours"), ElapsedHours },
        { QStringLiteral("ed"), ElapsedDays },   { QStringLiteral("eday"), ElapsedDays },
        { QStringLiteral("edays"), ElapsedDays },
        { QStringLiteral("ew"), ElapsedWeeks },  { QStringLiteral("ewk"), ElapsedWeeks },
        { QStringLiteral("ewks"), ElapsedWeeks },
        { QStringLiteral("eweek"), ElapsedWeeks },
        { QStringLiteral("eweeks"), ElapsedWeeks },
        { QStringLiteral("emo"), ElapsedMonths }, { QStringLiteral("emon"), ElapsedMonths },
        { QStringLiteral("emons"), ElapsedMonths },
        { QStringLiteral("emonth"), ElapsedMonths },
        { QStringLiteral("emonths"), ElapsedMonths },
    };
    return map;
}

// Up to two decimals, trailing zeros trimmed ("2", "2.5", "2.25").
QString trimmedNumber(double v)
{
    QString s = QString::number(v, 'f', 2);
    while (s.endsWith(QLatin1Char('0')))
        s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
        s.chop(1);
    return s;
}

} // namespace

QString format(qint64 millis, int unit)
{
    unit = normalizeUnit(unit);
    const double v = fromMillis(millis, unit);
    const bool plural = !qFuzzyCompare(std::abs(v) + 1.0, 2.0);   // |v| != 1
    return trimmedNumber(v) + QLatin1Char(' ')
           + QLatin1String(abbreviation(unit, plural));
}

bool parse(const QString &text, qint64 *millis, int *unit, int defaultUnit)
{
    static const QRegularExpression re(QStringLiteral(
        "^\\s*([+-]?)\\s*(\\d+(?:[.,]\\d+)?)\\s*([A-Za-z]*)\\s*$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return false;

    QString num = m.captured(2);
    num.replace(QLatin1Char(','), QLatin1Char('.'));   // accept the locale comma too
    bool numOk = false;
    double value = num.toDouble(&numOk);
    if (!numOk)
        return false;
    if (m.captured(1) == QLatin1String("-"))
        value = -value;

    int u = normalizeUnit(defaultUnit);
    const QString suffix = m.captured(3).toLower();
    if (!suffix.isEmpty()) {
        const auto it = suffixMap().constFind(suffix);
        if (it == suffixMap().constEnd())
            return false;
        u = it.value();
    }

    if (millis)
        *millis = toMillis(value, u);
    if (unit)
        *unit = u;
    return true;
}

} // namespace Duration
} // namespace schedule
