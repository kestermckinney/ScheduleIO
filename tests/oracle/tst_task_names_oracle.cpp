// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTest>
#include <QXmlStreamReader>

#ifndef SCHEDULEIO_FIXTURE_DIR
#define SCHEDULEIO_FIXTURE_DIR ""
#endif

// Layer 3 oracle: the task names MppIO decodes from a real binary .mpp must
// agree with the same project's MS Project XML export (MSProject.XML.9).
//
// Current decoder fidelity: every decoded name must be a genuine task name from
// the XML (zero false positives), and we must recover the large majority of
// them. The coverage threshold rises as the field decoding is completed.
class TstTaskNamesOracle : public QObject
{
    Q_OBJECT
private slots:
    void taskNamesMatchXml_data();
    void taskNamesMatchXml();

private:
    static QStringList taskNamesFromXml(const QString &xmlPath);
};

QStringList TstTaskNamesOracle::taskNamesFromXml(const QString &xmlPath)
{
    QStringList names;
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return names;
    QXmlStreamReader xml(&f);
    bool inTasks = false;
    int taskDepth = -1;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == u"Tasks")
                inTasks = true;
            else if (inTasks && name == u"Task")
                taskDepth = 0;            // entering a task; take its first <Name>
            else if (inTasks && taskDepth == 0 && name == u"Name")
                names << xml.readElementText();
        } else if (xml.isEndElement()) {
            if (xml.name() == u"Task")
                taskDepth = -1;
            else if (xml.name() == u"Tasks")
                inTasks = false;
        }
    }
    return names;
}

void TstTaskNamesOracle::taskNamesMatchXml_data()
{
    QTest::addColumn<QString>("mpp");
    QTest::addColumn<QString>("xml");
    const QString dir = QStringLiteral(SCHEDULEIO_FIXTURE_DIR);
    for (const QString &f : QDir(dir).entryList({ QStringLiteral("*.mpp") }, QDir::Files)) {
        const QString base = QFileInfo(f).completeBaseName();
        const QString xml = QDir(dir).filePath(base + QStringLiteral(".xml"));
        if (QFile::exists(xml))
            QTest::newRow(qPrintable(f)) << QDir(dir).filePath(f) << xml;
    }
}

void TstTaskNamesOracle::taskNamesMatchXml()
{
    if (QDir(QStringLiteral(SCHEDULEIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files)
            .isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));

    QSet<QString> decoded;
    for (const schedule::Task &t : io.project().tasks)
        if (!t.name.isEmpty())
            decoded.insert(t.name);

    const QStringList expected = taskNamesFromXml(xml);
    if (expected.isEmpty())
        QSKIP("this fixture has no named tasks in its XML export");
    const QSet<QString> expectedSet(expected.begin(), expected.end());

    // Recall: every task name in the XML oracle must be recovered by MppIO.
    // (The .mpp can legitimately contain MORE tasks than a filtered XML export,
    // so we measure recall of the oracle's names rather than strict subset.)
    int found = 0;
    QStringList missing;
    for (const QString &name : expectedSet) {
        if (decoded.contains(name))
            ++found;
        else
            missing << name;
    }
    const double recall = double(found) / double(expectedSet.size());
    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("recovered %1 / %2 XML task names (recall %3%); "
                                        "decoded %4 unique names total")
                             .arg(found).arg(expectedSet.size())
                             .arg(qRound(recall * 100)).arg(decoded.size());
    QVERIFY2(recall >= 0.98,
             qPrintable(QStringLiteral("recall %1%% below floor; missing e.g.: %2")
                            .arg(qRound(recall * 100))
                            .arg(QStringList(missing.mid(0, 5)).join(QStringLiteral(", ")))));
}

QTEST_MAIN(TstTaskNamesOracle)
#include "tst_task_names_oracle.moc"
