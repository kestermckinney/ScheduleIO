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

// Layer 3 oracle: task and resource notes. MppIO stores the *raw RTF* source while
// the Microsoft Project XML export stores the *plain text*, so we assert that every
// (non-empty) line of the XML note text is recovered verbatim inside the decoded
// RTF. This proves the notes were decoded and associated with the right entity.
class TstNotesOracle : public QObject
{
    Q_OBJECT
private slots:
    void notesMatchXml_data();
    void notesMatchXml();

private:
    static void parse(const QString &xmlPath, QHash<int, QString> &tasks, QHash<int, QString> &resources);
};

void TstNotesOracle::parse(const QString &xmlPath, QHash<int, QString> &tasks, QHash<int, QString> &resources)
{
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return;
    QXmlStreamReader xml(&f);
    enum Section { None, Task, Resource } sect = None;
    int uid = -1;
    QString notes;
    bool notesSeen = false;
    auto store = [&](QHash<int, QString> &h) { if (uid >= 0 && notesSeen) h.insert(uid, notes); };
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView n = xml.name();
            if (n == u"Task")          { sect = Task; uid = -1; notes.clear(); notesSeen = false; }
            else if (n == u"Resource") { sect = Resource; uid = -1; notes.clear(); notesSeen = false; }
            else if (sect != None && n == u"UID" && uid < 0) uid = xml.readElementText().toInt();
            else if (sect != None && n == u"Notes" && !notesSeen) { notes = xml.readElementText(); notesSeen = true; }
        } else if (xml.isEndElement()) {
            if (xml.name() == u"Task")          { store(tasks); sect = None; }
            else if (xml.name() == u"Resource") { store(resources); sect = None; }
        }
    }
}

void TstNotesOracle::notesMatchXml_data()
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

// True if every non-empty line of the plain-text note appears in the decoded RTF.
static bool rtfContainsPlain(const QString &rtf, const QString &plain)
{
    if (rtf.isEmpty())
        return false;
    const QStringList lines = plain.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString &line : lines)
        if (!rtf.contains(line.trimmed()))
            return false;
    return true;
}

void TstNotesOracle::notesMatchXml()
{
    if (QDir(QStringLiteral(MPPIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files).isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    QHash<int, QString> xTasks, xRes;
    parse(xml, xTasks, xRes);
    if (xTasks.isEmpty() && xRes.isEmpty())
        QSKIP("this fixture has no notes in its XML export");

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));

    QHash<int, QString> tNotes, rNotes;
    for (const MppTask &t : io.project().tasks)     tNotes.insert(t.uniqueId, t.notes);
    for (const MppResource &r : io.project().resources) rNotes.insert(r.uniqueId, r.notes);

    int tN = 0, tOk = 0, rN = 0, rOk = 0;
    for (auto it = xTasks.constBegin(); it != xTasks.constEnd(); ++it) {
        if (it.value().trimmed().isEmpty()) continue;
        ++tN;
        if (rtfContainsPlain(tNotes.value(it.key()), it.value())) ++tOk;
        else qInfo().noquote() << "  TASK note miss uid" << it.key() << "xml" << it.value()
                               << "decoded" << tNotes.value(it.key()).left(80);
    }
    for (auto it = xRes.constBegin(); it != xRes.constEnd(); ++it) {
        if (it.value().trimmed().isEmpty()) continue;
        ++rN;
        if (rtfContainsPlain(rNotes.value(it.key()), it.value())) ++rOk;
        else qInfo().noquote() << "  RES note miss uid" << it.key() << "xml" << it.value()
                               << "decoded" << rNotes.value(it.key()).left(80);
    }

    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("task notes %1/%2, resource notes %3/%4").arg(tOk).arg(tN).arg(rOk).arg(rN);
    QVERIFY2(tN + rN > 0, "no notes intersect XML");
    if (tN > 0) QVERIFY2(tOk == tN, "task notes not fully recovered from RTF");
    if (rN > 0) QVERIFY2(rOk == rN, "resource notes not fully recovered from RTF");
}

QTEST_MAIN(TstNotesOracle)
#include "tst_notes_oracle.moc"
