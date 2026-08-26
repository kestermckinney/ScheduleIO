// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/fielddecoders.h"

#include <QTest>
#include <QtEndian>

#include <cstring>
#include <ctime>

using namespace FieldDecoders;

class TstFieldDecoders : public QObject
{
    Q_OBJECT
private slots:
    void timestampRoundTrip_data();
    void timestampRoundTrip();
    void timestampsAreWallClock_data();
    void timestampsAreWallClock();
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
    // MPP timestamps are timezone-less wall clock, carried as LocalTime so they compare
    // directly against the dates the scheduling calendar computes.
    QCOMPARE(dt.timeSpec(), Qt::LocalTime);
    QCOMPARE(encodeTimestampSeconds(dt), minutes);
}

void TstFieldDecoders::timestampsAreWallClock_data()
{
    QTest::addColumn<QByteArray>("zone");
    for (const char *tz : { "UTC", "America/New_York", "Asia/Tokyo", "Pacific/Honolulu" })
        QTest::newRow(tz) << QByteArray(tz);
}

// The wall clock a file names must survive decoding unchanged in every timezone, and
// re-encode to the same bytes. Before wall-clock handling these came back as UTC-spec
// instants, so the moment anything read them as local time -- the scheduler's calendar,
// or a QDateTime crossing into QML -- the date slid by the zone offset.
void TstFieldDecoders::timestampsAreWallClock()
{
    QFETCH(QByteArray, zone);
    struct ZoneGuard {
        QByteArray saved = qgetenv("TZ");
        ~ZoneGuard() { saved.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", saved); tzset(); }
    } guard;
    qputenv("TZ", zone);
    tzset();

    // 1984-01-01 08:00 + 30 days == 1984-01-31 08:00, whatever the local zone is.
    const quint32 secs = 30u * 86400u + 8u * 3600u;
    const QDateTime dt = decodeTimestampSeconds(secs);
    QCOMPARE(dt.date(), QDate(1984, 1, 31));
    QCOMPARE(dt.time(), QTime(8, 0));
    QCOMPARE(encodeTimestampSeconds(dt), secs);

    // The same wall clock built by hand (as the app or the XML reader would) must encode
    // identically -- that equivalence is what makes file dates and computed dates mix.
    QCOMPARE(encodeTimestampSeconds(QDateTime(QDate(1984, 1, 31), QTime(8, 0))), secs);

    const qint32 tenths = qint32(secs / 6);
    QByteArray buf(4, '\0');
    qToLittleEndian<qint32>(tenths, buf.data());
    const QDateTime viaTenths = decodeTimestampTenths(buf, 0);
    QCOMPARE(viaTenths.date(), QDate(1984, 1, 31));
    QCOMPARE(viaTenths.time(), QTime(8, 0));
    QCOMPARE(encodeTimestampTenths(viaTenths), tenths);

    const quint32 packed = encodeMppTimestamp(QDateTime(QDate(2026, 7, 16), QTime(8, 0)));
    QByteArray mpp(4, '\0');
    qToLittleEndian<quint32>(packed, mpp.data());
    const QDateTime viaMpp = decodeMppTimestamp(mpp, 0);
    QCOMPARE(viaMpp.date(), QDate(2026, 7, 16));
    QCOMPARE(viaMpp.time(), QTime(8, 0));
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
    const QUuid u(QStringLiteral("{00112233-4455-6677-8899-aabbccddeeff}"));
    const QByteArray bytes = encodeGuid(u);
    QCOMPARE(bytes, QByteArray::fromHex("33221100554477668899aabbccddeeff"));
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
