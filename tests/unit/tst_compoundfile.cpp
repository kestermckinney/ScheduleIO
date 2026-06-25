// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "ole/compoundfile.h"

#include <QTest>

class TstCompoundFile : public QObject
{
    Q_OBJECT
private slots:
    void rejectsNonCfb();
    void roundTripMiniStreams();
    void roundTripBigStreamCrossesMiniCutoff();
    void roundTripNestedStorages();
    void garbageDoesNotCrash_data();
    void garbageDoesNotCrash();
};

void TstCompoundFile::rejectsNonCfb()
{
    CompoundFile cf;
    QVERIFY(!cf.openFromData(QByteArray("not an ole file at all, really")));
    QVERIFY(!cf.isValid());
    QVERIFY(!cf.errorString().isEmpty());
    QVERIFY(!CompoundFile::looksLikeCompoundFile(QByteArray("xxxx")));
}

void TstCompoundFile::roundTripMiniStreams()
{
    CompoundFile w;
    w.addStream({ QStringLiteral("Project"), QStringLiteral("Props") }, QByteArray("\x0e\x00", 2));
    w.addStream({ QStringLiteral("Project"), QStringLiteral("Task"), QStringLiteral("FixedData") },
                QByteArray("small fixed data"));
    const QByteArray bytes = w.toByteArray();
    QVERIFY(!bytes.isEmpty());
    QVERIFY(CompoundFile::looksLikeCompoundFile(bytes));

    CompoundFile r;
    QVERIFY2(r.openFromData(bytes), qPrintable(r.errorString()));
    QVERIFY(r.hasStorage({ QStringLiteral("Project") }));
    QVERIFY(r.hasStorage({ QStringLiteral("Project"), QStringLiteral("Task") }));
    QVERIFY(r.hasStream({ QStringLiteral("Project"), QStringLiteral("Props") }));
    QCOMPARE(r.readStream({ QStringLiteral("Project"), QStringLiteral("Props") }),
             QByteArray("\x0e\x00", 2));
    QCOMPARE(r.readStream({ QStringLiteral("Project"), QStringLiteral("Task"),
                            QStringLiteral("FixedData") }),
             QByteArray("small fixed data"));
}

void TstCompoundFile::roundTripBigStreamCrossesMiniCutoff()
{
    // Streams >= 4096 bytes must use regular sectors, < 4096 the mini stream.
    QByteArray big(10000, 'B');
    for (int i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>(i & 0xFF);
    QByteArray small(100, 's');

    CompoundFile w;
    w.addStream({ QStringLiteral("big") }, big);
    w.addStream({ QStringLiteral("small") }, small);
    const QByteArray bytes = w.toByteArray();

    CompoundFile r;
    QVERIFY2(r.openFromData(bytes), qPrintable(r.errorString()));
    QCOMPARE(r.readStream({ QStringLiteral("big") }), big);
    QCOMPARE(r.readStream({ QStringLiteral("small") }), small);
}

void TstCompoundFile::roundTripNestedStorages()
{
    CompoundFile w;
    w.addStream({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"),
                  QStringLiteral("leaf") }, QByteArray("deep"));
    w.addStream({ QStringLiteral("A"), QStringLiteral("sibling") }, QByteArray("flat"));
    const QByteArray bytes = w.toByteArray();

    CompoundFile r;
    QVERIFY2(r.openFromData(bytes), qPrintable(r.errorString()));
    QCOMPARE(r.readStream({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"),
                            QStringLiteral("leaf") }), QByteArray("deep"));
    QCOMPARE(r.readStream({ QStringLiteral("A"), QStringLiteral("sibling") }), QByteArray("flat"));
    QVERIFY(r.childNames({ QStringLiteral("A") }).contains(QStringLiteral("B")));
}

void TstCompoundFile::garbageDoesNotCrash_data()
{
    QTest::addColumn<QByteArray>("input");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("short") << QByteArray(10, '\xd0');
    // Valid signature, but everything after is garbage.
    QByteArray badHeader(512, '\xff');
    badHeader.replace(0, 8, QByteArray("\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8));
    QTest::newRow("sig only, garbage header") << badHeader;
}

void TstCompoundFile::garbageDoesNotCrash()
{
    QFETCH(QByteArray, input);
    CompoundFile cf;
    // The contract: never crash, just return false on malformed input.
    cf.openFromData(input);
    QVERIFY(true);
}

QTEST_APPLESS_MAIN(TstCompoundFile)
#include "tst_compoundfile.moc"
