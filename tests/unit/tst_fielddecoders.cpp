// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/fielddecoders.h"

#include <QTest>
#include <QtEndian>

#include <cstring>

using namespace FieldDecoders;

class TstFieldDecoders : public QObject
{
    Q_OBJECT
private slots:
    void timestampRoundTrip_data();
    void timestampRoundTrip();
    void durationRoundTrip();
    void percentClampsAndScales();
    void guidRoundTrip();
    void unicodeStringRoundTrip();
    void doubleAndWork();
    void shortBuffersFailCleanly();
};

void TstFieldDecoders::timestampRoundTrip_data()
{
    QTest::addColumn<quint32>("minutes");
    QTest::newRow("epoch") << quint32(0);
    QTest::newRow("one hour") << quint32(60);
    QTest::newRow("one day") << quint32(1440);
    QTest::newRow("large") << quint32(20'000'000);
}

void TstFieldDecoders::timestampRoundTrip()
{
    QFETCH(quint32, minutes);
    const QDateTime dt = decodeTimestampSeconds(minutes);
    QCOMPARE(dt.timeSpec(), Qt::UTC);
    QCOMPARE(encodeTimestampSeconds(dt), minutes);
}

void TstFieldDecoders::durationRoundTrip()
{
    for (qint32 raw : { 0, 10, 600, 12345, -50 }) {
        const qint64 ms = decodeDurationTenthMinutes(raw);
        QCOMPARE(encodeDurationTenthMinutes(ms), raw);
    }
}

void TstFieldDecoders::percentClampsAndScales()
{
    QCOMPARE(encodePercent(0.0), quint16(0));
    QCOMPARE(encodePercent(1.0), quint16(100));
    QCOMPARE(encodePercent(2.0), quint16(100));   // clamped
    QCOMPARE(encodePercent(-1.0), quint16(0));     // clamped
    QVERIFY(qFuzzyCompare(decodePercent(50) + 1.0, 0.5 + 1.0));
}

void TstFieldDecoders::guidRoundTrip()
{
    const QUuid u = QUuid::createUuid();
    const QByteArray bytes = encodeGuid(u);
    QCOMPARE(bytes.size(), 16);
    QCOMPARE(decodeGuid(bytes), u);
}

void TstFieldDecoders::unicodeStringRoundTrip()
{
    for (const QString &s : { QStringLiteral("Task A"),
                              QStringLiteral(""),
                              QStringLiteral("Ünïcødé — 日本語") }) {
        const QByteArray enc = encodeUnicodeString(s);
        int consumed = 0;
        const QString back = decodeUnicodeString(enc, 0, &consumed);
        QCOMPARE(back, s);
        if (!s.isEmpty())
            QCOMPARE(consumed, enc.size());
    }
}

void TstFieldDecoders::doubleAndWork()
{
    // readDouble round-trips an 8-byte LE double, e.g. a currency amount.
    QByteArray buf(8, '\0');
    const double cost = 395999.85;
    quint64 bits;
    std::memcpy(&bits, &cost, 8);
    qToLittleEndian<quint64>(bits, reinterpret_cast<uchar *>(buf.data()));
    double out = 0.0;
    QVERIFY(readDouble(buf, 0, &out));
    QVERIFY(qFuzzyCompare(out, cost));
    QVERIFY(!readDouble(buf, 4, &out));   // would overrun

    // Work: 480000 thousandths-of-minute == 8 hours.
    QCOMPARE(decodeWorkDouble(480000.0), qint64(8) * 60 * 60 * 1000);
    QCOMPARE(decodeWorkDouble(0.0), qint64(0));
}

void TstFieldDecoders::shortBuffersFailCleanly()
{
    QByteArray tiny(2, '\0');
    quint32 u = 0;
    QVERIFY(!readU32(tiny, 0, &u));
    QVERIFY(!readU32(tiny, -4, &u));
    quint16 s = 0;
    QVERIFY(readU16(tiny, 0, &s));
}

QTEST_APPLESS_MAIN(TstFieldDecoders)
#include "tst_fielddecoders.moc"
