// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QTest>
#include <QXmlStreamReader>

#ifndef MPPIO_FIXTURE_DIR
#define MPPIO_FIXTURE_DIR ""
#endif

// Layer 3 oracle: saved baselines decoded from var data must agree with the nested
// <Baseline> blocks of the MS Project XML export, matched per (task UID, baseline
// number). MS Project stores baselines as <Baseline><Number>n</Number>... inside
// each <Task>; n == 0 is the current baseline, 1..10 are saved baselines.
class TstBaselineOracle : public QObject
{
    Q_OBJECT
private slots:
    void baselinesMatchXml_data();
    void baselinesMatchXml();

private:
    struct XmlBaseline { int number = -1; double cost = 0; qint64 workMs = -1; qint64 durMs = -1;
                         QDateTime start, finish; bool costSeen = false; };
    // uid -> (number -> baseline)
    static QHash<int, QHash<int, XmlBaseline>> parse(const QString &xmlPath);
    static qint64 isoDur(const QString &s);
};

qint64 TstBaselineOracle::isoDur(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("PT(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?"));
    const QRegularExpressionMatch m = re.match(s);
    if (!m.hasMatch())
        return -1;
    return ((m.captured(1).toLongLong() * 3600) + (m.captured(2).toLongLong() * 60)
            + m.captured(3).toLongLong()) * 1000;
}

QHash<int, QHash<int, TstBaselineOracle::XmlBaseline>> TstBaselineOracle::parse(const QString &xmlPath)
{
    QHash<int, QHash<int, XmlBaseline>> out;
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QXmlStreamReader xml(&f);
    bool inTask = false, inBaseline = false;
    int uid = -1;
    XmlBaseline cur;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView n = xml.name();
            if (n == u"Task") { inTask = true; uid = -1; }
            else if (inTask && n == u"UID" && uid < 0 && !inBaseline) uid = xml.readElementText().toInt();
            else if (inTask && n == u"Baseline") { inBaseline = true; cur = XmlBaseline(); }
            else if (inBaseline && n == u"Number") cur.number = xml.readElementText().toInt();
            else if (inBaseline && n == u"Cost") { cur.cost = xml.readElementText().toDouble(); cur.costSeen = true; }
            else if (inBaseline && n == u"Work") cur.workMs = isoDur(xml.readElementText());
            else if (inBaseline && n == u"Duration") cur.durMs = isoDur(xml.readElementText());
            else if (inBaseline && n == u"Start") cur.start = QDateTime::fromString(xml.readElementText(), Qt::ISODate);
            else if (inBaseline && n == u"Finish") cur.finish = QDateTime::fromString(xml.readElementText(), Qt::ISODate);
        } else if (xml.isEndElement()) {
            const QStringView n = xml.name();
            if (n == u"Baseline") { if (uid >= 0 && cur.number >= 0) out[uid].insert(cur.number, cur); inBaseline = false; }
            else if (n == u"Task") inTask = false;
        }
    }
    return out;
}

void TstBaselineOracle::baselinesMatchXml_data()
{
    QTest::addColumn<QString>("mpp");
    QTest::addColumn<QString>("xml");
    const QString dir = QStringLiteral(MPPIO_FIXTURE_DIR);
    for (const QString &f : QDir(dir).entryList({ QStringLiteral("*.mpp") }, QDir::Files)) {
        const QString xml = QDir(dir).filePath(QFileInfo(f).completeBaseName() + QStringLiteral(".xml"));
        if (QFile::exists(xml))
            QTest::newRow(qPrintable(f)) << QDir(dir).filePath(f) << xml;
    }
}

static bool sameInstant(const QDateTime &decoded, const QDateTime &xml)
{
    return decoded.isValid() && xml.isValid()
        && decoded.date() == xml.date() && decoded.time() == xml.time();
}

void TstBaselineOracle::baselinesMatchXml()
{
    if (QDir(QStringLiteral(MPPIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files).isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));

    const QHash<int, QHash<int, XmlBaseline>> expected = parse(xml);
    int totalBaselines = 0;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it)
        totalBaselines += it.value().size();
    if (totalBaselines == 0)
        QSKIP("this fixture has no saved baselines in its XML export");

    // Decoded: uid -> (number -> baseline)
    QHash<int, QHash<int, MppBaseline>> decoded;
    for (const MppTask &t : io.project().tasks)
        for (const MppBaseline &b : t.baselines)
            decoded[t.uniqueId].insert(b.number, b);

    int startN = 0, startOk = 0, finishN = 0, finishOk = 0;
    int costN = 0, costOk = 0, workN = 0, workOk = 0, durN = 0, durOk = 0;
    for (auto uit = expected.constBegin(); uit != expected.constEnd(); ++uit) {
        const auto dmap = decoded.constFind(uit.key());
        if (dmap == decoded.constEnd())
            continue;
        for (auto bit = uit.value().constBegin(); bit != uit.value().constEnd(); ++bit) {
            const auto d = dmap->constFind(bit.key());
            if (d == dmap->constEnd())
                continue;
            const XmlBaseline &x = bit.value();
            if (x.start.isValid())  { ++startN;  if (sameInstant(d->start, x.start)) ++startOk; }
            if (x.finish.isValid()) { ++finishN; if (sameInstant(d->finish, x.finish)) ++finishOk; }
            if (x.costSeen)         { ++costN;   if (qAbs(d->cost - x.cost) < 0.05) ++costOk; }
            if (x.workMs >= 0)      { ++workN;   if (d->workMillis == x.workMs) ++workOk; }
            if (x.durMs >= 0)       { ++durN;    if (d->durationMillis == x.durMs) ++durOk; }
        }
    }

    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("baseline Start %1/%2 Finish %3/%4 Cost %5/%6 Work %7/%8 Dur %9/%10")
                             .arg(startOk).arg(startN).arg(finishOk).arg(finishN)
                             .arg(costOk).arg(costN).arg(workOk).arg(workN).arg(durOk).arg(durN);

    // At least the baseline dates must be recovered for the matched baselines.
    QVERIFY2(startN + finishN > 0, "no baseline dates intersect XML (matching failed)");
    if (startN > 0)  QVERIFY2(double(startOk) / startN >= 0.90, "baseline Start disagrees with XML");
    if (finishN > 0) QVERIFY2(double(finishOk) / finishN >= 0.90, "baseline Finish disagrees with XML");
    if (costN > 0)   QVERIFY2(double(costOk) / costN >= 0.90, "baseline Cost disagrees with XML");
    if (workN > 0)   QVERIFY2(double(workOk) / workN >= 0.90, "baseline Work disagrees with XML");
    if (durN > 0)    QVERIFY2(double(durOk) / durN >= 0.90, "baseline Duration disagrees with XML");
}

QTEST_MAIN(TstBaselineOracle)
#include "tst_baseline_oracle.moc"
