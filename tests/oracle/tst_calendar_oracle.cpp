// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QTest>
#include <QXmlStreamReader>

#include <algorithm>

#include "fixtureutils.h"

// Layer 3 oracle: calendar working hours (per weekday) and exceptions decoded from
// the CALENDAR_DATA blob must agree with the Microsoft Project XML export.
class TstCalendarOracle : public QObject
{
    Q_OBJECT
private slots:
    void calendarsMatchXml_data();
    void calendarsMatchXml();

public:
    struct Range { QTime start; QTime end; bool operator==(const Range &o) const { return start == o.start && end == o.end; } bool operator<(const Range &o) const { return start < o.start; } };
    struct XmlExc { QString name; bool working = false; QDate from; QList<Range> times; };
    struct XmlCal { QList<Range> week[7]; };   // 0=Monday..6=Sunday

private:
    static void parse(const QString &xmlPath, QHash<QString, XmlCal> &cals, QList<XmlExc> &excs);
};

void TstCalendarOracle::parse(const QString &xmlPath, QHash<QString, XmlCal> &cals, QList<XmlExc> &excs)
{
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return;
    QXmlStreamReader xml(&f);
    QString calName;
    XmlCal cur;
    bool inCal = false, inExc = false, inWeekDay = false;
    int dayType = -1, dayWorking = -1;
    QList<Range> times;
    QTime from;
    XmlExc exc;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView n = xml.name();
            if (n == u"Calendar") { inCal = true; calName.clear(); cur = XmlCal(); }
            else if (inCal && !inExc && !inWeekDay && n == u"Name" && calName.isEmpty()) calName = xml.readElementText();
            else if (n == u"Exception") { inExc = true; exc = XmlExc(); times.clear(); }
            else if (n == u"WeekDay") { inWeekDay = true; dayType = -1; dayWorking = -1; times.clear(); }
            else if (n == u"DayType") dayType = xml.readElementText().toInt();
            else if (n == u"DayWorking") dayWorking = xml.readElementText().toInt();
            else if (inExc && n == u"Name") exc.name = xml.readElementText();
            else if (inExc && n == u"FromDate") exc.from = QDateTime::fromString(xml.readElementText(), Qt::ISODate).date();
            else if (n == u"FromTime") from = QTime::fromString(xml.readElementText(), QStringLiteral("HH:mm:ss"));
            else if (n == u"ToTime") times.append({ from, QTime::fromString(xml.readElementText(), QStringLiteral("HH:mm:ss")) });
        } else if (xml.isEndElement()) {
            const QStringView n = xml.name();
            if (n == u"WeekDay") {
                if (dayType >= 1 && dayType <= 7) {
                    const int idx = (dayType == 1) ? 6 : (dayType - 2);   // Sun..Sat -> Mon0..Sun6
                    cur.week[idx] = times;
                }
                inWeekDay = false;
            } else if (n == u"Exception") {
                exc.working = (dayWorking == 1);
                exc.times = times;
                if (!exc.name.isEmpty()) excs.append(exc);
                inExc = false;
            } else if (n == u"Calendar") {
                if (!calName.isEmpty()) cals.insert(calName, cur);
                inCal = false;
            }
        }
    }
}

void TstCalendarOracle::calendarsMatchXml_data()
{
    QTest::addColumn<QString>("mpp");
    QTest::addColumn<QString>("xml");
    for (const QString &mpp : fixtures::mppFiles()) {
        const QString xml = fixtures::xmlSibling(mpp);
        if (!xml.isEmpty())
            QTest::newRow(qPrintable(fixtures::label(mpp))) << mpp << xml;
    }
}

static QList<TstCalendarOracle::Range> toRanges(const QList<schedule::TimeRange> &in)
{
    QList<TstCalendarOracle::Range> out;
    for (const schedule::TimeRange &r : in) out.append({ r.start, r.end });
    std::sort(out.begin(), out.end());
    return out;
}

void TstCalendarOracle::calendarsMatchXml()
{
    if (fixtures::mppFiles().isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    QHash<QString, XmlCal> xmlCals;
    QList<XmlExc> xmlExcs;
    parse(xml, xmlCals, xmlExcs);

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));
    QHash<QString, schedule::Calendar> decoded;
    for (const schedule::Calendar &c : io.project().calendars)
        if (!decoded.contains(c.name))
            decoded.insert(c.name, c);

    // --- working hours: per matched calendar, compare each weekday's ranges ---
    int dayN = 0, dayOk = 0;
    for (auto it = xmlCals.constBegin(); it != xmlCals.constEnd(); ++it) {
        const auto d = decoded.constFind(it.key());
        if (d == decoded.constEnd() || d->workingTimes.size() != 7)
            continue;
        for (int day = 0; day < 7; ++day) {
            QList<Range> x = it.value().week[day];
            std::sort(x.begin(), x.end());
            ++dayN;
            if (toRanges(d->workingTimes.at(day)) == x) ++dayOk;
        }
    }

    // --- exceptions: each XML exception recovered (matched by name) ---
    QList<schedule::CalendarException> decExc;
    for (const schedule::Calendar &c : io.project().calendars)
        decExc.append(c.exceptions);
    int exN = 0, exOk = 0;
    for (const XmlExc &xe : xmlExcs) {
        ++exN;
        for (const schedule::CalendarException &de : decExc) {
            if (de.name == xe.name && de.working == xe.working
                && (!xe.from.isValid() || de.fromDate == xe.from)
                && toRanges(de.workingTimes) == [&]{ QList<Range> r = xe.times; std::sort(r.begin(), r.end()); return r; }()) {
                ++exOk; break;
            }
        }
    }

    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("weekday hours %1/%2, exceptions %3/%4").arg(dayOk).arg(dayN).arg(exOk).arg(exN);
    QVERIFY2(dayN > 0, "no calendars intersect XML by name");
    QVERIFY2(double(dayOk) / dayN >= 0.95, "calendar weekday working hours disagree with XML");
    if (exN > 0)
        QVERIFY2(exOk == exN, "calendar exceptions disagree with XML");
}

QTEST_MAIN(TstCalendarOracle)
#include "tst_calendar_oracle.moc"
