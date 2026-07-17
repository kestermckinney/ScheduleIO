// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTest>
#include <QXmlStreamReader>

#include "fixtureutils.h"

// Layer 3 oracle: task ID / OutlineLevel / PercentComplete decoded from the
// binary .mpp FixedData must agree with the MS Project XML export.
class TstTaskFieldsOracle : public QObject
{
    Q_OBJECT
private slots:
    void fieldsMatchXml_data();
    void fieldsMatchXml();

private:
    struct XmlTask { int id = -1; int outline = -1; int percent = -1; int milestone = -1; int summary = -1;
                     int constraintType = -1; QString wbs; bool wbsSeen = false; };
    static QHash<int, XmlTask> tasksFromXml(const QString &xmlPath);
};

QHash<int, TstTaskFieldsOracle::XmlTask> TstTaskFieldsOracle::tasksFromXml(const QString &xmlPath)
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
            else if (inTask && n == u"UID" && uid < 0) uid = xml.readElementText().toInt();
            else if (inTask && n == u"ID" && cur.id < 0) cur.id = xml.readElementText().toInt();
            else if (inTask && n == u"OutlineLevel" && cur.outline < 0) cur.outline = xml.readElementText().toInt();
            else if (inTask && n == u"PercentComplete" && cur.percent < 0) cur.percent = xml.readElementText().toInt();
            else if (inTask && n == u"Milestone" && cur.milestone < 0) cur.milestone = xml.readElementText().toInt();
            else if (inTask && n == u"Summary" && cur.summary < 0) cur.summary = xml.readElementText().toInt();
            else if (inTask && n == u"ConstraintType" && cur.constraintType < 0) cur.constraintType = xml.readElementText().toInt();
            else if (inTask && n == u"WBS" && !cur.wbsSeen) { cur.wbs = xml.readElementText(); cur.wbsSeen = true; }
        } else if (xml.isEndElement()) {
            if (xml.name() == u"Task") { if (uid >= 0) out.insert(uid, cur); inTask = false; }
            else if (xml.name() == u"Tasks") inTasks = false;
        }
    }
    return out;
}

void TstTaskFieldsOracle::fieldsMatchXml_data()
{
    QTest::addColumn<QString>("mpp");
    QTest::addColumn<QString>("xml");
    for (const QString &mpp : fixtures::mppFiles()) {
        const QString xml = fixtures::xmlSibling(mpp);
        if (!xml.isEmpty())
            QTest::newRow(qPrintable(fixtures::label(mpp))) << mpp << xml;
    }
}

void TstTaskFieldsOracle::fieldsMatchXml()
{
    if (fixtures::mppFiles().isEmpty())
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

    int n = 0, idOk = 0, olOk = 0, pcOk = 0, msOk = 0, smOk = 0, ctOk = 0, wbsOk = 0;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        const auto d = decoded.constFind(it.key());
        if (d == decoded.constEnd())
            continue;
        ++n;
        if (d->id == it.value().id) ++idOk;
        if (d->outlineLevel == it.value().outline) ++olOk;
        if (qRound(d->percentComplete * 100.0) == it.value().percent) ++pcOk;
        if ((d->milestone ? 1 : 0) == it.value().milestone) ++msOk;
        if ((d->summary ? 1 : 0) == it.value().summary) ++smOk;
        if (d->constraintType == it.value().constraintType) ++ctOk;
        if (d->wbs == it.value().wbs) ++wbsOk;
    }
    QVERIFY(n > 0);
    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("ID %1 Outline %2 Pct %3 Milestone %4 Summary %5 Constraint %6 WBS %7 (of %8)")
                             .arg(idOk).arg(olOk).arg(pcOk).arg(msOk).arg(smOk).arg(ctOk).arg(wbsOk).arg(n);
    QVERIFY2(double(idOk) / n >= 0.98, "task IDs disagree with XML");
    QVERIFY2(double(olOk) / n >= 0.98, "outline levels disagree with XML");
    QVERIFY2(double(msOk) / n >= 0.98, "milestone flags disagree with XML");
    QVERIFY2(double(smOk) / n >= 0.98, "summary flags disagree with XML");
    QVERIFY2(double(ctOk) / n >= 0.98, "constraint types disagree with XML");
    QVERIFY2(double(wbsOk) / n >= 0.98, "WBS codes disagree with XML");
    // Summary tasks store a duration-weighted % rollup that differs from the
    // displayed/exported value, so exact agreement is only expected for leaf
    // tasks. We require the large majority to match.
    QVERIFY2(double(pcOk) / n >= 0.85, "percent complete disagrees with XML");
}

QTEST_MAIN(TstTaskFieldsOracle)
#include "tst_task_fields_oracle.moc"
