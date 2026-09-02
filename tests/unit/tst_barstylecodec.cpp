// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Unit tests for the 195-byte STYLE_DATA bar-style record (de)serialiser.

#include "codec/barstylecodec.h"
#include "model/viewstyles.h"

#include <QTest>

class TstBarStyleCodec : public QObject
{
    Q_OBJECT

private slots:
    void defaultsRoundTrip();
    void everyFieldRoundTrips();
    void colourAutomaticVsExplicitBlack();
    void nameIsFixedWidthAndTruncates();
    void reservedGapBytesArePreserved();
    void outOfRangeOffsetIsSafe();
};

// A 195-byte buffer padded so writeRecord/readRecord have room at `off`.
static QByteArray blank(int off = 0)
{
    return QByteArray(off + BarStyleCodec::kRecordSize, '\0');
}

void TstBarStyleCodec::defaultsRoundTrip()
{
    const schedule::ViewBarStyle def;
    QByteArray d = blank();
    BarStyleCodec::writeRecord(d, 0, def);
    const schedule::ViewBarStyle back = BarStyleCodec::readRecord(d, 0);
    QCOMPARE(back, def);
}

void TstBarStyleCodec::everyFieldRoundTrips()
{
    schedule::ViewBarStyle s;
    s.name = QStringLiteral("Critical Path");
    s.middleShape = 7;
    s.middlePattern = 8;
    s.middleColor = 0x8ABBED;
    s.middleFlag = 3;
    s.startShape = 28;            // packed: shape 3, type 1
    s.startColor = 0x101010;
    s.endShape = 11;
    s.endColor = 0x546A7B;
    s.fromField = qint32(0x0B400029u);   // ACTUAL_START
    s.toField = qint32(0x0B400077u);
    s.showFor = 0x0000080000000001ULL;
    s.showForNot = 0x0000100000000000ULL;
    s.row = 3;
    s.barText = { -1, qint32(0x0B400031u), -1, -1, qint32(0x0B40000Eu) };
    s.styleId = 17;
    s.flag87 = 1;

    QByteArray d = blank(40);   // exercise a non-zero base offset too
    BarStyleCodec::writeRecord(d, 40, s);
    QCOMPARE(BarStyleCodec::readRecord(d, 40), s);
}

void TstBarStyleCodec::colourAutomaticVsExplicitBlack()
{
    schedule::ViewBarStyle s;
    s.middleColor = schedule::TextStyle::kAutomatic;   // "Automatic"
    s.startColor = 0x000000;                           // explicit black
    s.endColor = 0xFFFFFF;

    QByteArray d = blank();
    BarStyleCodec::writeRecord(d, 0, s);
    // Automatic is stored as 00 00 00 FF (flag byte set); black as 00 00 00 00.
    QCOMPARE(quint8(d.at(2 + 3)), quint8(0xFF));
    QCOMPARE(quint8(d.at(16 + 3)), quint8(0x00));

    const schedule::ViewBarStyle back = BarStyleCodec::readRecord(d, 0);
    QCOMPARE(back.middleColor, schedule::TextStyle::kAutomatic);
    QCOMPARE(back.startColor, 0x000000);
    QCOMPARE(back.endColor, 0xFFFFFF);
}

void TstBarStyleCodec::nameIsFixedWidthAndTruncates()
{
    // readRecord stops at the NUL terminator, so a short name written over a
    // long one still reads back correctly (the field tail is left untouched --
    // MS Project pads it with 0xFF on styles it authors, and preserving that
    // keeps a round-trip byte-exact).
    QByteArray d = blank();
    schedule::ViewBarStyle longName;
    longName.name = QStringLiteral("*Manual Summary Rollup (Warning)");
    BarStyleCodec::writeRecord(d, 0, longName);
    schedule::ViewBarStyle shortName;
    shortName.name = QStringLiteral("Task");
    BarStyleCodec::writeRecord(d, 0, shortName);
    QCOMPARE(BarStyleCodec::readRecord(d, 0).name, QStringLiteral("Task"));

    // A pre-existing 0xFF-padded tail (MS Project's convention) survives a write.
    QByteArray padded(BarStyleCodec::kRecordSize, char(0xFF));
    BarStyleCodec::writeRecord(padded, 0, shortName);
    QCOMPARE(BarStyleCodec::readRecord(padded, 0).name, QStringLiteral("Task"));
    QCOMPARE(quint8(padded.at(91 + 10)), quint8(0xFF));   // tail past "Task\0" untouched

    // Over-long names are clipped to the record's ~51-char field.
    schedule::ViewBarStyle huge;
    huge.name = QString(80, QChar('x'));
    BarStyleCodec::writeRecord(d, 0, huge);
    QVERIFY(BarStyleCodec::readRecord(d, 0).name.size() <= 51);
}

void TstBarStyleCodec::reservedGapBytesArePreserved()
{
    QByteArray d = blank();
    // Mark the record's undecoded gap regions (+6..13, +20..27, +33..40) with a
    // sentinel; writeRecord must leave every one of them alone.
    for (int o : { 6, 7, 8, 9, 10, 11, 12, 13, 20, 21, 22, 23, 24, 25, 26, 27,
                   33, 34, 35, 36, 37, 38, 39, 40 })
        d[o] = char(0xAB);
    schedule::ViewBarStyle s;
    s.name = QStringLiteral("Task");
    s.middleShape = 1;
    BarStyleCodec::writeRecord(d, 0, s);
    for (int o : { 6, 7, 8, 9, 10, 11, 12, 13, 20, 21, 22, 23, 24, 25, 26, 27,
                   33, 34, 35, 36, 37, 38, 39, 40 })
        QCOMPARE(quint8(d.at(o)), quint8(0xAB));
}

void TstBarStyleCodec::outOfRangeOffsetIsSafe()
{
    QByteArray tiny(10, '\0');
    // No crash, no write.
    BarStyleCodec::writeRecord(tiny, 0, schedule::ViewBarStyle{});
    QCOMPARE(tiny, QByteArray(10, '\0'));
    QCOMPARE(BarStyleCodec::readRecord(tiny, 0), schedule::ViewBarStyle{});
    QCOMPARE(BarStyleCodec::readRecord(blank(), -4), schedule::ViewBarStyle{});
}

QTEST_APPLESS_MAIN(TstBarStyleCodec)
#include "tst_barstylecodec.moc"
