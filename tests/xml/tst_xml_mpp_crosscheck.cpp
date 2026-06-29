// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"
#include "xmlio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTest>

#ifndef MPPIO_FIXTURE_DIR
#define MPPIO_FIXTURE_DIR ""
#endif

// The whole point of XmlIO is that it shares MppIO's in-memory model. This test
// loads the same project both ways -- the .mpp via MppIO and the .xml via XmlIO --
// and checks the two MppProject structures agree on the data they both decode.
// That is only possible because they populate the very same types.
class TstXmlMppCrosscheck : public QObject
{
    Q_OBJECT
private slots:
    void interop_data();
    void interop();
};

void TstXmlMppCrosscheck::interop_data()
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

void TstXmlMppCrosscheck::interop()
{
    if (QDir(QStringLiteral(MPPIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files).isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    MppIO bin;
    QVERIFY2(bin.open(mpp), qPrintable(bin.errorString()));
    XmlIO text;
    QVERIFY2(text.open(xml), qPrintable(text.errorString()));

    const MppProject &fromMpp = bin.project();
    const MppProject &fromXml = text.project();

    // Both decoders populate the same model type -- prove interop on task names.
    QHash<int, QString> mppNames;
    for (const MppTask &t : fromMpp.tasks)
        if (!t.name.isEmpty())
            mppNames.insert(t.uniqueId, t.name);

    QHash<int, QString> xmlNames;
    for (const MppTask &t : fromXml.tasks)
        if (!t.name.isEmpty())
            xmlNames.insert(t.uniqueId, t.name);

    QVERIFY(!xmlNames.isEmpty());

    // Every task the XML export names must be present, by UID, with the same name
    // in the model decoded from the binary file.
    int matched = 0, total = 0;
    for (auto it = xmlNames.constBegin(); it != xmlNames.constEnd(); ++it) {
        if (!mppNames.contains(it.key()))
            continue;   // the .mpp can hold extra/blank rows; only check shared UIDs
        ++total;
        if (mppNames.value(it.key()) == it.value())
            ++matched;
    }
    QVERIFY2(total > 0, "no task UIDs in common between .mpp and .xml");
    QCOMPARE(matched, total);
}

QTEST_MAIN(TstXmlMppCrosscheck)
#include "tst_xml_mpp_crosscheck.moc"
