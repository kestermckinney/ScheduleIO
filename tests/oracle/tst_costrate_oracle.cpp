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

// Layer 3 oracle: resource cost-rate tables decoded from var data must agree with
// the <Rates>/<Rate> entries in the Microsoft Project XML export. We validate the
// *current* standard rate per resource (the headline value most callers want) and
// the full set of standard-rate values per resource (time-phased changes).
class TstCostRateOracle : public QObject
{
    Q_OBJECT
public:
    struct XmlRate { int table = 0; QDateTime from; double standard = 0.0; };

private slots:
    void costRatesMatchXml_data();
    void costRatesMatchXml();

private:
    static QHash<int, QList<XmlRate>> parse(const QString &xmlPath);
};

QHash<int, QList<TstCostRateOracle::XmlRate>> TstCostRateOracle::parse(const QString &xmlPath)
{
    QHash<int, QList<XmlRate>> out;
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QXmlStreamReader xml(&f);
    bool inRes = false, inRate = false;
    int uid = -1;
    XmlRate cur;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView n = xml.name();
            if (n == u"Resource") { inRes = true; uid = -1; }
            else if (inRes && n == u"UID" && uid < 0 && !inRate) uid = xml.readElementText().toInt();
            else if (inRes && n == u"Rate") { inRate = true; cur = XmlRate(); }
            else if (inRate && n == u"RateTable") cur.table = xml.readElementText().toInt();
            else if (inRate && n == u"RatesFrom") cur.from = QDateTime::fromString(xml.readElementText(), Qt::ISODate);
            else if (inRate && n == u"StandardRate") cur.standard = xml.readElementText().toDouble();
        } else if (xml.isEndElement()) {
            const QStringView n = xml.name();
            if (n == u"Rate") { if (uid >= 0) out[uid].append(cur); inRate = false; }
            else if (n == u"Resource") inRes = false;
        }
    }
    return out;
}

void TstCostRateOracle::costRatesMatchXml_data()
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

// The "current" standard rate is the table-0 entry with the latest RatesFrom (XML)
// / the open-ended or latest entry (decoded).
static double xmlCurrentRate(QList<TstCostRateOracle::XmlRate> rates)
{
    double best = 0.0; QDateTime bestFrom;
    for (const auto &r : rates) {
        if (r.table != 0) continue;
        if (!bestFrom.isValid() || r.from > bestFrom) { bestFrom = r.from; best = r.standard; }
    }
    return best;
}
static double decodedCurrentRate(const QList<schedule::CostRate> &rates)
{
    double openRate = 0.0; bool haveOpen = false;
    double latest = 0.0; QDateTime latestEnd;
    for (const schedule::CostRate &r : rates) {
        if (r.table != 0) continue;
        if (!r.endDate.isValid()) { openRate = r.standardRate; haveOpen = true; }
        else if (!latestEnd.isValid() || r.endDate > latestEnd) { latestEnd = r.endDate; latest = r.standardRate; }
    }
    return haveOpen ? openRate : latest;
}

void TstCostRateOracle::costRatesMatchXml()
{
    if (QDir(QStringLiteral(SCHEDULEIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files).isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    const QHash<int, QList<XmlRate>> expected = parse(xml);
    bool anyNonZero = false;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it)
        for (const auto &r : it.value())
            if (r.standard != 0.0) { anyNonZero = true; break; }
    if (!anyNonZero)
        QSKIP("this fixture has no resource cost rates in its XML export");

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));
    QHash<int, QList<schedule::CostRate>> decoded;
    for (const schedule::Resource &r : io.project().resources)
        decoded.insert(r.uniqueId, r.costRates);

    // (a) current standard rate per resource with a non-zero XML current rate.
    int curN = 0, curOk = 0;
    // (b) full set of distinct non-zero standard rates per resource.
    int setN = 0, setOk = 0;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        const double xc = xmlCurrentRate(it.value());
        if (xc != 0.0) {
            ++curN;
            const double dc = decodedCurrentRate(decoded.value(it.key()));
            if (qAbs(dc - xc) < 0.01) ++curOk;
            else qInfo().noquote() << "  CUR rate miss uid" << it.key() << "xml" << xc << "decoded" << dc;
        }

        QList<double> xs, ds;
        for (const auto &r : it.value()) if (r.table == 0 && r.standard != 0.0) xs << r.standard;
        for (const schedule::CostRate &r : decoded.value(it.key())) if (r.table == 0 && r.standardRate != 0.0) ds << r.standardRate;
        if (!xs.isEmpty()) {
            std::sort(xs.begin(), xs.end());
            std::sort(ds.begin(), ds.end());
            ++setN;
            if (xs == ds) ++setOk;
        }
    }

    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("current rate %1/%2, rate-set %3/%4").arg(curOk).arg(curN).arg(setOk).arg(setN);
    QVERIFY2(curN > 0, "no non-zero resource cost rates intersect XML");
    QVERIFY2(double(curOk) / curN >= 0.98, "current resource standard rates disagree with XML");
    if (setN > 0)
        QVERIFY2(double(setOk) / setN >= 0.95, "resource standard-rate sets disagree with XML");
}

QTEST_MAIN(TstCostRateOracle)
#include "tst_costrate_oracle.moc"
