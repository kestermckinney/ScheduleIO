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

#ifndef SCHEDULEIO_FIXTURE_DIR
#define SCHEDULEIO_FIXTURE_DIR ""
#endif

// Layer 3 oracle: resource Availability-table rows decoded from var data must
// agree with the <AvailabilityPeriods>/<AvailabilityPeriod> entries in the
// Microsoft Project XML export. Fixture: mpp14availability.mpp/.xml, sourced
// from MPXJ's own test suite (github.com/joniles/mpxj, junit/data/, LGPL-2.1) --
// none of this project's other fixtures exercise resource availability.
class TstAvailabilityOracle : public QObject
{
    Q_OBJECT
public:
    struct XmlPeriod { QDateTime from; QDateTime to; double units = 0.0; };

private slots:
    void availabilityMatchesXml_data();
    void availabilityMatchesXml();

private:
    static QHash<int, QList<XmlPeriod>> parse(const QString &xmlPath);
};

QHash<int, QList<TstAvailabilityOracle::XmlPeriod>> TstAvailabilityOracle::parse(const QString &xmlPath)
{
    QHash<int, QList<XmlPeriod>> out;
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QXmlStreamReader xml(&f);
    bool inRes = false, inPeriod = false;
    int uid = -1;
    XmlPeriod cur;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView n = xml.name();
            if (n == u"Resource") { inRes = true; uid = -1; }
            else if (inRes && n == u"UID" && uid < 0 && !inPeriod) uid = xml.readElementText().toInt();
            else if (inRes && n == u"AvailabilityPeriod") { inPeriod = true; cur = XmlPeriod(); }
            else if (inPeriod && n == u"AvailableFrom") cur.from = QDateTime::fromString(xml.readElementText(), Qt::ISODate);
            else if (inPeriod && n == u"AvailableTo") cur.to = QDateTime::fromString(xml.readElementText(), Qt::ISODate);
            else if (inPeriod && n == u"AvailableUnits") cur.units = xml.readElementText().toDouble();
        } else if (xml.isEndElement()) {
            const QStringView n = xml.name();
            if (n == u"AvailabilityPeriod") { if (uid >= 0) out[uid].append(cur); inPeriod = false; }
            else if (n == u"Resource") inRes = false;
        }
    }
    return out;
}

void TstAvailabilityOracle::availabilityMatchesXml_data()
{
    QTest::addColumn<QString>("mpp");
    QTest::addColumn<QString>("xml");
    const QString dir = QStringLiteral(SCHEDULEIO_FIXTURE_DIR);
    for (const QString &f : QDir(dir).entryList({ QStringLiteral("*.mpp") }, QDir::Files)) {
        const QString xml = QDir(dir).filePath(QFileInfo(f).completeBaseName() + QStringLiteral(".xml"));
        if (QFile::exists(xml))
            QTest::newRow(qPrintable(f)) << QDir(dir).filePath(f) << xml;
    }
}

// decoded is UTC wall-clock; xml is naive local wall-clock. Compare components.
static bool sameInstant(const QDateTime &decoded, const QDateTime &xml)
{
    return decoded.isValid() && xml.isValid()
        && decoded.date() == xml.date() && decoded.time() == xml.time();
}

void TstAvailabilityOracle::availabilityMatchesXml()
{
    if (QDir(QStringLiteral(SCHEDULEIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files).isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    const QHash<int, QList<XmlPeriod>> expected = parse(xml);
    int xmlTotal = 0;
    for (const auto &periods : expected)
        xmlTotal += periods.size();
    if (xmlTotal == 0)
        QSKIP("this fixture has no resource availability periods in its XML export");

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));
    QHash<int, QList<schedule::AvailabilityPeriod>> decoded;
    for (const schedule::Resource &r : io.project().resources)
        decoded.insert(r.uniqueId, r.availabilityTable);

    int ok = 0, total = 0;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        const QList<schedule::AvailabilityPeriod> dec = decoded.value(it.key());
        for (const XmlPeriod &xp : it.value()) {
            ++total;
            bool matched = false;
            for (const schedule::AvailabilityPeriod &dp : dec) {
                if (!sameInstant(dp.startDate, xp.from))
                    continue;
                // XML AvailableTo is inclusive-minute like our decoded endDate.
                if (!sameInstant(dp.endDate, xp.to))
                    continue;
                if (qAbs(dp.units - xp.units) > 0.001)
                    continue;
                matched = true;
                break;
            }
            if (matched)
                ++ok;
            else
                qInfo().noquote() << "  availability miss: from" << xp.from << "to" << xp.to << "units" << xp.units;
        }
    }

    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("availability periods %1/%2").arg(ok).arg(total);
    QVERIFY2(total > 0, "no resource availability periods intersect XML");
    QVERIFY2(double(ok) / total >= 0.98, "resource availability periods disagree with XML");
}

QTEST_MAIN(TstAvailabilityOracle)
#include "tst_availability_oracle.moc"
