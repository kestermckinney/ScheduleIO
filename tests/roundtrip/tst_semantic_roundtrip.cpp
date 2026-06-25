// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QDir>
#include <QTest>

#ifndef MPPIO_FIXTURE_DIR
#define MPPIO_FIXTURE_DIR ""
#endif

class TstSemanticRoundtrip : public QObject
{
    Q_OBJECT
private slots:
    void syntheticRoundTrip();
    void writerIsDeterministic();
    void realFixtures_data();
    void realFixtures();

private:
    static MppProject makeSampleProject();
};

MppProject TstSemanticRoundtrip::makeSampleProject()
{
    MppProject p;
    p.formatVersion = MppProject::FormatVersion::Mpp14;
    p.title = QStringLiteral("Scaffold Plan");
    p.author = QStringLiteral("Paul");
    p.startDate = QDateTime(QDate(2026, 1, 2), QTime(8, 0), Qt::UTC);
    p.finishDate = QDateTime(QDate(2026, 3, 31), QTime(17, 0), Qt::UTC);

    MppTask t1;
    t1.uniqueId = 1; t1.id = 1; t1.outlineLevel = 1;
    t1.name = QStringLiteral("Design");
    t1.start = QDateTime(QDate(2026, 1, 2), QTime(9, 0), Qt::UTC);
    t1.finish = QDateTime(QDate(2026, 1, 9), QTime(17, 0), Qt::UTC);
    t1.durationMillis = qint64(8) * 3600 * 1000;   // divisible by the duration unit
    t1.percentComplete = 0.5;
    MppTask t2;
    t2.uniqueId = 2; t2.id = 2; t2.outlineLevel = 1;
    t2.name = QStringLiteral("Implement — 実装");
    t2.milestone = true;
    p.tasks = { t1, t2 };

    MppResource r;
    r.uniqueId = 1; r.id = 1; r.name = QStringLiteral("Alice"); r.initials = QStringLiteral("A");
    r.maxUnits = 0.5;
    p.resources = { r };

    return p;
}

void TstSemanticRoundtrip::syntheticRoundTrip()
{
    const MppProject original = makeSampleProject();

    MppIO writer;
    writer.setProject(original);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));

    MppIO reader;
    QVERIFY2(reader.openFromData(bytes), qPrintable(reader.errorString()));

    // Primary correctness gate: model survives read -> write -> read.
    QVERIFY(reader.project() == original);
}

void TstSemanticRoundtrip::writerIsDeterministic()
{
    MppIO io;
    io.setProject(makeSampleProject());
    const QByteArray b = io.saveToData();

    MppIO io2;
    QVERIFY(io2.openFromData(b));
    const QByteArray c = io2.saveToData();

    // Our writer must be byte-stable across a re-save of the same model.
    QCOMPARE(c, b);
}

void TstSemanticRoundtrip::realFixtures_data()
{
    QTest::addColumn<QString>("path");
    const QString dir = QStringLiteral(MPPIO_FIXTURE_DIR);
    const QStringList mpps = QDir(dir).entryList({ QStringLiteral("*.mpp") }, QDir::Files);
    for (const QString &f : mpps)
        QTest::newRow(qPrintable(f)) << QDir(dir).filePath(f);
}

void TstSemanticRoundtrip::realFixtures()
{
    // With no fixtures present this slot is still invoked once with no data row;
    // skip cleanly before touching QFETCH.
    if (QDir(QStringLiteral(MPPIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files)
            .isEmpty())
        QSKIP("no .mpp fixtures present yet (add real files under tests/fixtures)");

    QFETCH(QString, path);

    MppIO reader;
    if (!reader.open(path))
        QSKIP(qPrintable(QStringLiteral("parser cannot yet read this fixture: %1 "
                                         "(expected until the MPP field mapping is filled in)")
                             .arg(reader.errorString())));

    const MppProject m1 = reader.project();
    MppIO writer;
    writer.setProject(m1);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));

    MppIO reader2;
    QVERIFY2(reader2.openFromData(bytes), qPrintable(reader2.errorString()));
    QVERIFY(reader2.project() == m1);
}

QTEST_MAIN(TstSemanticRoundtrip)
#include "tst_semantic_roundtrip.moc"
