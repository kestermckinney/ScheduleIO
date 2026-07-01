// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTest>
#include <QXmlStreamReader>

#ifndef SCHEDULEIO_FIXTURE_DIR
#define SCHEDULEIO_FIXTURE_DIR ""
#endif

// Layer 3 oracle: resources and assignments decoded from the binary .mpp must
// agree with the MS Project XML export.
class TstEntitiesOracle : public QObject
{
    Q_OBJECT
private slots:
    void entitiesMatchXml_data();
    void entitiesMatchXml();
};

void TstEntitiesOracle::entitiesMatchXml_data()
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

void TstEntitiesOracle::entitiesMatchXml()
{
    if (QDir(QStringLiteral(SCHEDULEIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files).isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    // --- parse XML: resource names by UID, assignment (task,res) pairs ---
    QHash<int, QString> xmlResName;
    QHash<int, double> xmlResUnits;
    QSet<QPair<int, int>> xmlAssign;
    QSet<QPair<int, int>> xmlLinks;   // (predecessorUID, successorUID)
    QHash<QPair<int, int>, qint64> xmlLinkLag;   // (pred,succ) -> lag in ms
    QDateTime xmlStart, xmlFinish;
    QHash<int, QString> xmlCalName;
    QHash<int, int> xmlCalMask;   // calendar UID -> working-day mask (Mon bit0..Sun bit6)
    QString xmlTitle, xmlAuthor;
    bool haveTitle = false, haveAuthor = false;
    {
        QFile f(xml);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QXmlStreamReader r(&f);
        enum { None, Res, Assign, TaskSect, Cal } sect = None;
        int uid = -1, taskUid = -1, resUid = -1, curTaskUid = -1, predUid = -1, calUid = -1;
        qint64 linkLag = 0;   // lag of the current PredecessorLink, in ms
        int dayType = -1, dayWorking = -1, calMask = 0;
        double maxUnits = -1.0;
        QString name, calName;
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement()) {
                const QStringView n = r.name();
                if (n == u"Resource") { sect = Res; uid = -1; name.clear(); maxUnits = -1.0; }
                else if (n == u"Assignment") { sect = Assign; taskUid = resUid = -1; }
                else if (n == u"Task") { sect = TaskSect; curTaskUid = -1; }
                else if (n == u"Calendar") { sect = Cal; calUid = -1; calName.clear(); calMask = 0; }
                else if (sect == Cal && n == u"UID" && calUid < 0) calUid = r.readElementText().toInt();
                else if (sect == Cal && n == u"Name" && calName.isEmpty()) calName = r.readElementText();
                else if (sect == Cal && n == u"WeekDay") { dayType = -1; dayWorking = -1; }
                else if (sect == Cal && n == u"DayType") dayType = r.readElementText().toInt();
                else if (sect == Cal && n == u"DayWorking") dayWorking = r.readElementText().toInt();
                else if (sect == Res && n == u"UID" && uid < 0) uid = r.readElementText().toInt();
                else if (sect == Res && n == u"Name" && name.isEmpty()) name = r.readElementText();
                else if (sect == Res && n == u"MaxUnits" && maxUnits < 0) maxUnits = r.readElementText().toDouble();
                else if (sect == Assign && n == u"TaskUID" && taskUid < 0) taskUid = r.readElementText().toInt();
                else if (sect == Assign && n == u"ResourceUID" && resUid < 0) resUid = r.readElementText().toInt();
                else if (sect == TaskSect && n == u"UID" && curTaskUid < 0) curTaskUid = r.readElementText().toInt();
                else if (sect == TaskSect && n == u"PredecessorLink") { predUid = -1; linkLag = 0; }
                else if (sect == TaskSect && n == u"PredecessorUID") predUid = r.readElementText().toInt();
                else if (sect == TaskSect && n == u"LinkLag")
                    linkLag = r.readElementText().toLongLong() * 6000;   // tenths-of-minute -> ms
                else if (sect == None && n == u"StartDate" && !xmlStart.isValid())
                    xmlStart = QDateTime::fromString(r.readElementText(), Qt::ISODate);
                else if (sect == None && n == u"FinishDate" && !xmlFinish.isValid())
                    xmlFinish = QDateTime::fromString(r.readElementText(), Qt::ISODate);
                else if (sect == None && n == u"Title" && !haveTitle) { xmlTitle = r.readElementText(); haveTitle = true; }
                else if (sect == None && n == u"Author" && !haveAuthor) { xmlAuthor = r.readElementText(); haveAuthor = true; }
            } else if (r.isEndElement()) {
                if (r.name() == u"WeekDay") {
                    if (dayType >= 1 && dayType <= 7 && dayWorking == 1)
                        calMask |= 1 << ((dayType == 1) ? 6 : (dayType - 2));
                } else if (r.name() == u"Calendar") {
                    if (calUid >= 0 && !calName.isEmpty()) xmlCalName.insert(calUid, calName);
                    if (calUid >= 0) xmlCalMask.insert(calUid, calMask);
                    sect = None;
                }
                else if (r.name() == u"Resource") { if (uid >= 0 && !name.isEmpty()) { xmlResName.insert(uid, name); if (maxUnits >= 0) xmlResUnits.insert(uid, maxUnits); } sect = None; }
                else if (r.name() == u"Assignment") {
                    if (taskUid >= 0 && resUid >= 0) xmlAssign.insert({ taskUid, resUid });
                    sect = None;
                } else if (r.name() == u"PredecessorLink") {
                    if (predUid >= 0 && curTaskUid >= 0) {
                        xmlLinks.insert({ predUid, curTaskUid });
                        xmlLinkLag.insert({ predUid, curTaskUid }, linkLag);
                    }
                } else if (r.name() == u"Task") {
                    sect = None;
                }
            }
        }
    }

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));
    const schedule::Project &p = io.project();

    // --- resources: every named XML resource recovered with the right name ---
    QHash<int, QString> decResName;
    for (const schedule::Resource &r : p.resources)
        decResName.insert(r.uniqueId, r.name);
    int resFound = 0;
    for (auto it = xmlResName.constBegin(); it != xmlResName.constEnd(); ++it)
        if (decResName.value(it.key()) == it.value())
            ++resFound;

    // --- assignments: every XML (task,resource) link present in decoded ---
    QSet<QPair<int, int>> decAssign;
    for (const schedule::Assignment &a : p.assignments)
        decAssign.insert({ a.taskUniqueId, a.resourceUniqueId });
    int assignFound = 0;
    for (const auto &pr : xmlAssign)
        if (decAssign.contains(pr))
            ++assignFound;

    // --- predecessor links: every XML (pred,succ) present in decoded ---
    QSet<QPair<int, int>> decLinks;
    QHash<QPair<int, int>, qint64> decLinkLag;
    for (const schedule::Relation &r : p.relations) {
        decLinks.insert({ r.predecessorTaskUid, r.successorTaskUid });
        decLinkLag.insert({ r.predecessorTaskUid, r.successorTaskUid }, r.lagMillis);
    }
    int linkFound = 0, lagOk = 0, lagTotal = 0;
    for (const auto &pr : xmlLinks) {
        if (!decLinks.contains(pr))
            continue;
        ++linkFound;
        ++lagTotal;
        if (decLinkLag.value(pr) == xmlLinkLag.value(pr))
            ++lagOk;
    }

    // --- calendars: every XML calendar name recovered (matched by name set,
    // since calendar unique ids can differ from the export) ---
    QSet<QString> decCalNames;
    for (const schedule::Calendar &c : p.calendars)
        if (!c.name.isEmpty())
            decCalNames.insert(c.name);
    QSet<QString> xmlCalNames;
    for (auto it = xmlCalName.constBegin(); it != xmlCalName.constEnd(); ++it)
        xmlCalNames.insert(it.value());
    int calFound = 0;
    for (const QString &cn : xmlCalNames)
        if (decCalNames.contains(cn))
            ++calFound;

    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("resources %1/%2  assignments %3/%4  links %5/%6  lag %7/%8  calendars %9/%10")
                             .arg(resFound).arg(xmlResName.size())
                             .arg(assignFound).arg(xmlAssign.size())
                             .arg(linkFound).arg(xmlLinks.size())
                             .arg(lagOk).arg(lagTotal)
                             .arg(calFound).arg(xmlCalNames.size());

    if (!xmlResName.isEmpty())
        QVERIFY2(double(resFound) / xmlResName.size() >= 0.98, "resource names disagree with XML");

    // resource max units
    QHash<int, double> decResUnits;
    for (const schedule::Resource &r : p.resources)
        decResUnits.insert(r.uniqueId, r.maxUnits);
    int unitsOk = 0, unitsTotal = 0;
    for (auto it = xmlResUnits.constBegin(); it != xmlResUnits.constEnd(); ++it) {
        if (!decResUnits.contains(it.key()))
            continue;
        ++unitsTotal;
        if (qAbs(decResUnits.value(it.key()) - it.value()) < 0.001)
            ++unitsOk;
    }
    if (unitsTotal > 0)
        QVERIFY2(double(unitsOk) / unitsTotal >= 0.98,
                 qPrintable(QStringLiteral("resource max units: %1/%2").arg(unitsOk).arg(unitsTotal)));
    if (!xmlAssign.isEmpty())
        QVERIFY2(double(assignFound) / xmlAssign.size() >= 0.95, "assignment links disagree with XML");
    if (!xmlLinks.isEmpty())
        QVERIFY2(double(linkFound) / xmlLinks.size() >= 0.95, "predecessor links disagree with XML");
    if (lagTotal > 0)
        QVERIFY2(double(lagOk) / lagTotal >= 0.98,
                 qPrintable(QStringLiteral("predecessor link lag: %1/%2").arg(lagOk).arg(lagTotal)));
    if (!xmlCalNames.isEmpty())
        QVERIFY2(double(calFound) / xmlCalNames.size() >= 0.95, "calendar names disagree with XML");

    // Calendar working-day masks (only for XML calendars that list working days).
    QHash<int, int> decCalMask;
    for (const schedule::Calendar &c : p.calendars)
        decCalMask.insert(c.uniqueId, int(c.workingDayMask));
    int maskOk = 0, maskTotal = 0;
    for (auto it = xmlCalMask.constBegin(); it != xmlCalMask.constEnd(); ++it) {
        if (it.value() == 0 || !decCalMask.contains(it.key()))
            continue;
        ++maskTotal;
        if (decCalMask.value(it.key()) == it.value())
            ++maskOk;
    }
    if (maskTotal > 0)
        QVERIFY2(double(maskOk) / maskTotal >= 0.95,
                 qPrintable(QStringLiteral("calendar working-day masks: %1/%2").arg(maskOk).arg(maskTotal)));

    // Project Title / Author from the SummaryInformation property set.
    if (haveTitle && !xmlTitle.isEmpty())
        QVERIFY2(p.title == xmlTitle,
                 qPrintable(QStringLiteral("Title: decoded '%1' vs XML '%2'").arg(p.title, xmlTitle)));
    if (haveAuthor && !xmlAuthor.isEmpty())
        QVERIFY2(p.author == xmlAuthor,
                 qPrintable(QStringLiteral("Author: decoded '%1' vs XML '%2'").arg(p.author, xmlAuthor)));

    // Project start/finish dates (compare wall-clock components; decoded is UTC).
    if (xmlStart.isValid()) {
        QVERIFY2(p.startDate.isValid() && p.startDate.date() == xmlStart.date()
                     && p.startDate.time() == xmlStart.time(),
                 qPrintable(QStringLiteral("project StartDate: decoded %1 vs XML %2")
                                .arg(p.startDate.toString(Qt::ISODate), xmlStart.toString(Qt::ISODate))));
    }
    if (xmlFinish.isValid()) {
        QVERIFY2(p.finishDate.isValid() && p.finishDate.date() == xmlFinish.date()
                     && p.finishDate.time() == xmlFinish.time(),
                 qPrintable(QStringLiteral("project FinishDate: decoded %1 vs XML %2")
                                .arg(p.finishDate.toString(Qt::ISODate), xmlFinish.toString(Qt::ISODate))));
    }
}

QTEST_MAIN(TstEntitiesOracle)
#include "tst_entities_oracle.moc"
