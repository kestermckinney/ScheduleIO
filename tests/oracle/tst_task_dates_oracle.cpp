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

#ifndef SCHEDULEIO_FIXTURE_DIR
#define SCHEDULEIO_FIXTURE_DIR ""
#endif

// Layer 3 oracle: task Start / Finish / Duration decoded from the binary .mpp
// FixedData must agree with the MS Project XML export, matched by unique id.
class TstTaskDatesOracle : public QObject
{
    Q_OBJECT
private slots:
    void datesMatchXml_data();
    void datesMatchXml();

private:
    struct XmlTask { QDateTime start; QDateTime finish; qint64 durationMs = -1; bool manual = false; };
    static QHash<int, XmlTask> tasksFromXml(const QString &xmlPath);
    static qint64 parseIsoDuration(const QString &s);
};

qint64 TstTaskDatesOracle::parseIsoDuration(const QString &s)
{
    // e.g. PT640H0M0S
    static const QRegularExpression re(
        QStringLiteral("PT(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?"));
    const QRegularExpressionMatch m = re.match(s);
    if (!m.hasMatch())
        return -1;
    const qint64 h = m.captured(1).toLongLong();
    const qint64 mn = m.captured(2).toLongLong();
    const qint64 sec = m.captured(3).toLongLong();
    return ((h * 3600) + (mn * 60) + sec) * 1000;
}

QHash<int, TstTaskDatesOracle::XmlTask> TstTaskDatesOracle::tasksFromXml(const QString &xmlPath)
{
    QHash<int, XmlTask> out;
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QXmlStreamReader xml(&f);
    bool inTasks = false, inTask = false;
    int uid = -1;
    XmlTask cur;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView n = xml.name();
            if (n == u"Tasks") inTasks = true;
            else if (inTasks && n == u"Task") { inTask = true; uid = -1; cur = XmlTask(); }
            // Take the FIRST occurrence of each field; later ones belong to
            // nested <Baseline>/<PredecessorLink> sections, not the task itself.
            else if (inTask && n == u"UID" && uid < 0) uid = xml.readElementText().toInt();
            else if (inTask && n == u"Start" && !cur.start.isValid())
                cur.start = QDateTime::fromString(xml.readElementText(), Qt::ISODate);
            else if (inTask && n == u"Finish" && !cur.finish.isValid())
                cur.finish = QDateTime::fromString(xml.readElementText(), Qt::ISODate);
            else if (inTask && n == u"Duration" && cur.durationMs < 0)
                cur.durationMs = parseIsoDuration(xml.readElementText());
            else if (inTask && n == u"Manual")
                cur.manual = (xml.readElementText().toInt() != 0);
        } else if (xml.isEndElement()) {
            if (xml.name() == u"Task") { if (uid >= 0) out.insert(uid, cur); inTask = false; }
            else if (xml.name() == u"Tasks") inTasks = false;
        }
    }
    return out;
}

void TstTaskDatesOracle::datesMatchXml_data()
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

static bool sameInstant(const QDateTime &decoded, const QDateTime &xml)
{
    // decoded is UTC wall-clock; xml is naive local wall-clock. Compare components.
    return decoded.isValid() && xml.isValid()
        && decoded.date() == xml.date() && decoded.time() == xml.time();
}

void TstTaskDatesOracle::datesMatchXml()
{
    if (QDir(QStringLiteral(SCHEDULEIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files).isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));

    QHash<int, schedule::Task> decoded;
    for (const schedule::Task &t : io.project().tasks)
        decoded.insert(t.uniqueId, t);

    const QHash<int, XmlTask> expected = tasksFromXml(xml);
    QVERIFY2(!expected.isEmpty(), "no tasks parsed from XML oracle");

    int compared = 0, startOk = 0, finishOk = 0, durCompared = 0, durOk = 0;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        const auto d = decoded.constFind(it.key());
        if (d == decoded.constEnd())
            continue;   // task not decoded (e.g. a row with no name)
        if (!it.value().start.isValid())
            continue;
        Q_UNUSED(it.value().manual);   // both manual and auto scheduling are decoded
        ++compared;
        if (sameInstant(d->start, it.value().start)) ++startOk;
        if (sameInstant(d->finish, it.value().finish)) ++finishOk;
        if (it.value().durationMs >= 0) {
            ++durCompared;
            if (d->durationMillis == it.value().durationMs) ++durOk;
        }
    }

    QVERIFY2(compared > 0, "no overlapping tasks to compare");
    const double startRate = double(startOk) / compared;
    const double finishRate = double(finishOk) / compared;
    const double durRate = durCompared ? double(durOk) / durCompared : 1.0;
    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("start %1/%2 finish %3/%2 duration %4/%5")
                             .arg(startOk).arg(compared).arg(finishOk).arg(durOk).arg(durCompared);

    QVERIFY2(startRate >= 0.95, "task Start dates disagree with XML");
    QVERIFY2(finishRate >= 0.95, "task Finish dates disagree with XML");
    QVERIFY2(durRate >= 0.90, "task Durations disagree with XML");
}

QTEST_MAIN(TstTaskDatesOracle)
#include "tst_task_dates_oracle.moc"
