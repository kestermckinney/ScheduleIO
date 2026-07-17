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

#include "fixtureutils.h"

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
    for (const QString &mpp : fixtures::mppFiles()) {
        const QString xml = fixtures::xmlSibling(mpp);
        if (!xml.isEmpty())
            QTest::newRow(qPrintable(fixtures::label(mpp))) << mpp << xml;
    }
}

// Reduce RTF source to its plain text: resolve \uN? / \'hh escapes, map
// \par|\line|\tab to whitespace, drop other control words and group braces.
// Just enough fidelity for containment checks against the XML plain text.
static QString rtfToPlain(const QString &rtf)
{
    QString out;
    out.reserve(rtf.size());
    int i = 0;
    const int n = rtf.size();
    while (i < n) {
        const QChar c = rtf.at(i);
        if (c == QLatin1Char('{') || c == QLatin1Char('}')) {
            ++i;
            continue;
        }
        if (c != QLatin1Char('\\')) {
            if (c != QLatin1Char('\r') && c != QLatin1Char('\n'))
                out.append(c);
            ++i;
            continue;
        }
        // control: \\ \{ \} , \'hh , \uN? , or \word[-]N[ ]
        if (i + 1 >= n)
            break;
        const QChar next = rtf.at(i + 1);
        if (next == QLatin1Char('\\') || next == QLatin1Char('{') || next == QLatin1Char('}')) {
            out.append(next);
            i += 2;
            continue;
        }
        if (next == QLatin1Char('\'') && i + 3 < n) {
            const int hi = QString(rtf.at(i + 2)).toInt(nullptr, 16);
            const int lo = QString(rtf.at(i + 3)).toInt(nullptr, 16);
            out.append(QChar::fromLatin1(char(hi * 16 + lo)));   // cp1252 ~ latin1 for the test corpus
            i += 4;
            continue;
        }
        int j = i + 1;
        while (j < n && rtf.at(j).isLetter())
            ++j;
        const QString word = rtf.mid(i + 1, j - i - 1);
        int numStart = j;
        if (j < n && rtf.at(j) == QLatin1Char('-'))
            ++j;
        while (j < n && rtf.at(j).isDigit())
            ++j;
        const QString num = rtf.mid(numStart, j - numStart);
        if (j < n && rtf.at(j) == QLatin1Char(' '))
            ++j;   // control words eat one trailing space
        if (word == QLatin1String("u")) {
            int code = num.toInt();
            if (code < 0)
                code += 65536;
            out.append(QChar(ushort(code)));
            if (j < n && rtf.at(j) != QLatin1Char('\\'))
                ++j;   // skip the fallback character after \uN
        } else if (word == QLatin1String("par") || word == QLatin1String("line")) {
            out.append(QLatin1Char('\n'));
        } else if (word == QLatin1String("tab")) {
            out.append(QLatin1Char('\t'));
        }
        i = j;
    }
    return out;
}

// True if every non-empty line of the plain-text note appears in the decoded RTF.
static bool rtfContainsPlain(const QString &rtf, const QString &plain)
{
    if (rtf.isEmpty())
        return false;
    const QString text = rtfToPlain(rtf);
    const QStringList lines = plain.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString &line : lines)
        if (!text.contains(line.trimmed()))
            return false;
    return true;
}

void TstNotesOracle::notesMatchXml()
{
    if (fixtures::mppFiles().isEmpty())
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
    for (const schedule::Task &t : io.project().tasks)     tNotes.insert(t.uniqueId, t.notes);
    for (const schedule::Resource &r : io.project().resources) rNotes.insert(r.uniqueId, r.notes);

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
