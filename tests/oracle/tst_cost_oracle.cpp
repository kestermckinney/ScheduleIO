// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTest>
#include <QXmlStreamReader>

#ifndef MPPIO_FIXTURE_DIR
#define MPPIO_FIXTURE_DIR ""
#endif

// Layer 3 oracle: cost values (a plain currency double in the binary) decoded for
// tasks / resources / assignments must agree with the MS Project XML export. The
// task/assignment <Cost> we want is the entity-level one, NOT the <Cost> nested
// inside a <Baseline>, so we ignore everything while inside a <Baseline> element.
class TstCostOracle : public QObject
{
    Q_OBJECT
private slots:
    void costMatchesXml_data();
    void costMatchesXml();

private:
    struct XmlCost { double cost = 0; double fixed = 0; double actual = 0; double remaining = 0;
                     bool costSeen = false, fixedSeen = false, actualSeen = false, remainingSeen = false; };
    static void parse(const QString &xmlPath, QHash<int, XmlCost> &tasks,
                      QHash<int, XmlCost> &resources, QHash<int, XmlCost> &assignments);
};

void TstCostOracle::parse(const QString &xmlPath, QHash<int, XmlCost> &tasks,
                          QHash<int, XmlCost> &resources, QHash<int, XmlCost> &assignments)
{
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return;
    QXmlStreamReader xml(&f);
    enum Section { None, Task, Resource, Assignment } sect = None;
    bool inBaseline = false;
    int uid = -1;
    XmlCost cur;
    auto store = [&](QHash<int, XmlCost> &h) { if (uid >= 0) h.insert(uid, cur); };

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView n = xml.name();
            if (n == u"Task")            { sect = Task; uid = -1; cur = XmlCost(); }
            else if (n == u"Resource")   { sect = Resource; uid = -1; cur = XmlCost(); }
            else if (n == u"Assignment") { sect = Assignment; uid = -1; cur = XmlCost(); }
            else if (n == u"Baseline")   { inBaseline = true; }
            else if (sect != None && !inBaseline) {
                if (n == u"UID" && uid < 0) uid = xml.readElementText().toInt();
                else if (n == u"Cost" && !cur.costSeen) { cur.cost = xml.readElementText().toDouble(); cur.costSeen = true; }
                else if (n == u"FixedCost" && !cur.fixedSeen) { cur.fixed = xml.readElementText().toDouble(); cur.fixedSeen = true; }
                else if (n == u"ActualCost" && !cur.actualSeen) { cur.actual = xml.readElementText().toDouble(); cur.actualSeen = true; }
                else if (n == u"RemainingCost" && !cur.remainingSeen) { cur.remaining = xml.readElementText().toDouble(); cur.remainingSeen = true; }
            }
        } else if (xml.isEndElement()) {
            const QStringView n = xml.name();
            if (n == u"Baseline") inBaseline = false;
            else if (n == u"Task")       { store(tasks); sect = None; }
            else if (n == u"Resource")   { store(resources); sect = None; }
            else if (n == u"Assignment") { store(assignments); sect = None; }
        }
    }
}

void TstCostOracle::costMatchesXml_data()
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

static bool money(double a, double b) { return qAbs(a - b) < 0.05; }

void TstCostOracle::costMatchesXml()
{
    if (QDir(QStringLiteral(MPPIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files).isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));

    QHash<int, XmlCost> xTasks, xRes, xAssn;
    parse(xml, xTasks, xRes, xAssn);

    QHash<int, MppTask> tasks;
    for (const MppTask &t : io.project().tasks) tasks.insert(t.uniqueId, t);
    QHash<int, MppResource> res;
    for (const MppResource &r : io.project().resources) res.insert(r.uniqueId, r);

    // --- Tasks: Cost (+ FixedCost / ActualCost / RemainingCost where present) ---
    int n = 0, costOk = 0, fixedOk = 0, fixedN = 0, actOk = 0, actN = 0, remOk = 0, remN = 0;
    for (auto it = xTasks.constBegin(); it != xTasks.constEnd(); ++it) {
        const auto d = tasks.constFind(it.key());
        if (d == tasks.constEnd()) continue;
        ++n;
        if (money(d->cost, it->cost)) ++costOk;
        if (it->fixedSeen) { ++fixedN; if (money(d->fixedCost, it->fixed)) ++fixedOk; }
        if (it->actualSeen) { ++actN; if (money(d->actualCost, it->actual)) ++actOk; }
        if (it->remainingSeen) { ++remN; if (money(d->remainingCost, it->remaining)) ++remOk; }
    }
    QVERIFY2(n > 0, "no tasks intersect XML");
    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("task Cost %1/%2 Fixed %3/%4 Actual %5/%6 Remaining %7/%8")
                             .arg(costOk).arg(n).arg(fixedOk).arg(fixedN)
                             .arg(actOk).arg(actN).arg(remOk).arg(remN);
    QVERIFY2(double(costOk) / n >= 0.90, "task Cost disagrees with XML");
    if (fixedN > 0) QVERIFY2(double(fixedOk) / fixedN >= 0.90, "task FixedCost disagrees with XML");
    if (actN > 0)   QVERIFY2(double(actOk) / actN >= 0.90, "task ActualCost disagrees with XML");
    if (remN > 0)   QVERIFY2(double(remOk) / remN >= 0.90, "task RemainingCost disagrees with XML");

    // --- Resources: Cost ---
    int rn = 0, rCostOk = 0;
    for (auto it = xRes.constBegin(); it != xRes.constEnd(); ++it) {
        if (!it->costSeen) continue;
        const auto d = res.constFind(it.key());
        if (d == res.constEnd()) continue;
        ++rn;
        if (money(d->cost, it->cost)) ++rCostOk;
    }
    if (rn > 0) {
        qInfo().noquote() << QFileInfo(mpp).fileName() << QStringLiteral("resource Cost %1/%2").arg(rCostOk).arg(rn);
        QVERIFY2(double(rCostOk) / rn >= 0.90, "resource Cost disagrees with XML");
    }

    // --- Assignments: Cost (match by uniqueId) ---
    QHash<int, MppAssignment> assn;
    for (const MppAssignment &a : io.project().assignments) assn.insert(a.uniqueId, a);
    int an = 0, aCostOk = 0;
    for (auto it = xAssn.constBegin(); it != xAssn.constEnd(); ++it) {
        if (!it->costSeen) continue;
        const auto d = assn.constFind(it.key());
        if (d == assn.constEnd()) continue;
        ++an;
        if (money(d->cost, it->cost)) ++aCostOk;
    }
    if (an > 0) {
        qInfo().noquote() << QFileInfo(mpp).fileName() << QStringLiteral("assignment Cost %1/%2").arg(aCostOk).arg(an);
        QVERIFY2(double(aCostOk) / an >= 0.90, "assignment Cost disagrees with XML");
    }
}

QTEST_MAIN(TstCostOracle)
#include "tst_cost_oracle.moc"
