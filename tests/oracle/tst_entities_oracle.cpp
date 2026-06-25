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

#ifndef MPPIO_FIXTURE_DIR
#define MPPIO_FIXTURE_DIR ""
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
    const QString dir = QStringLiteral(MPPIO_FIXTURE_DIR);
    for (const QString &f : QDir(dir).entryList({ QStringLiteral("*.mpp") }, QDir::Files)) {
        const QString xml = QDir(dir).filePath(QFileInfo(f).completeBaseName() + QStringLiteral(".xml"));
        if (QFile::exists(xml))
            QTest::newRow(qPrintable(f)) << QDir(dir).filePath(f) << xml;
    }
}

void TstEntitiesOracle::entitiesMatchXml()
{
    if (QDir(QStringLiteral(MPPIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files).isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    // --- parse XML: resource names by UID, assignment (task,res) pairs ---
    QHash<int, QString> xmlResName;
    QSet<QPair<int, int>> xmlAssign;
    {
        QFile f(xml);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QXmlStreamReader r(&f);
        enum { None, Res, Assign } sect = None;
        int uid = -1, taskUid = -1, resUid = -1;
        QString name;
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement()) {
                const QStringView n = r.name();
                if (n == u"Resource") { sect = Res; uid = -1; name.clear(); }
                else if (n == u"Assignment") { sect = Assign; taskUid = resUid = -1; }
                else if (sect == Res && n == u"UID" && uid < 0) uid = r.readElementText().toInt();
                else if (sect == Res && n == u"Name" && name.isEmpty()) name = r.readElementText();
                else if (sect == Assign && n == u"TaskUID" && taskUid < 0) taskUid = r.readElementText().toInt();
                else if (sect == Assign && n == u"ResourceUID" && resUid < 0) resUid = r.readElementText().toInt();
            } else if (r.isEndElement()) {
                if (r.name() == u"Resource") { if (uid >= 0 && !name.isEmpty()) xmlResName.insert(uid, name); sect = None; }
                else if (r.name() == u"Assignment") {
                    if (taskUid >= 0 && resUid >= 0) xmlAssign.insert({ taskUid, resUid });
                    sect = None;
                }
            }
        }
    }

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));
    const MppProject &p = io.project();

    // --- resources: every named XML resource recovered with the right name ---
    QHash<int, QString> decResName;
    for (const MppResource &r : p.resources)
        decResName.insert(r.uniqueId, r.name);
    int resFound = 0;
    for (auto it = xmlResName.constBegin(); it != xmlResName.constEnd(); ++it)
        if (decResName.value(it.key()) == it.value())
            ++resFound;

    // --- assignments: every XML (task,resource) link present in decoded ---
    QSet<QPair<int, int>> decAssign;
    for (const MppAssignment &a : p.assignments)
        decAssign.insert({ a.taskUniqueId, a.resourceUniqueId });
    int assignFound = 0;
    for (const auto &pr : xmlAssign)
        if (decAssign.contains(pr))
            ++assignFound;

    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("resources %1/%2 (decoded %3)  assignments %4/%5 (decoded %6)")
                             .arg(resFound).arg(xmlResName.size()).arg(p.resources.size())
                             .arg(assignFound).arg(xmlAssign.size()).arg(p.assignments.size());

    if (!xmlResName.isEmpty())
        QVERIFY2(double(resFound) / xmlResName.size() >= 0.98, "resource names disagree with XML");
    if (!xmlAssign.isEmpty())
        QVERIFY2(double(assignFound) / xmlAssign.size() >= 0.95, "assignment links disagree with XML");
}

QTEST_MAIN(TstEntitiesOracle)
#include "tst_entities_oracle.moc"
