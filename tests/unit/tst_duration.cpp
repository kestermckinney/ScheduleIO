// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/duration.h"

#include <QtTest>

using namespace schedule;

class tst_duration : public QObject
{
    Q_OBJECT
private slots:
    void cleanup() { Duration::resetWorkingTimeProfile(); }
    void unitConversions();
    void formatting_data();
    void formatting();
    void parsing_data();
    void parsing();
    void parseRejects();
    void normalization();
    void workingTimeProfileScalesDayWeekMonth();
};

void tst_duration::workingTimeProfileScalesDayWeekMonth()
{
    // File > Options > Calendar: hours per day / week, days per month.
    Duration::setWorkingTimeProfile(450 /*7.5h*/, 2250 /*37.5h*/, 22);
    const qint64 hour = 3600LL * 1000LL;
    QCOMPARE(Duration::toMillis(1, Duration::Days), qint64(7.5 * hour));
    QCOMPARE(Duration::toMillis(1, Duration::Weeks), qint64(37.5 * hour));
    QCOMPARE(Duration::toMillis(1, Duration::Months), qint64(22 * 7.5 * hour));
    QCOMPARE(Duration::format(qint64(15 * hour), Duration::Days), QStringLiteral("2 days"));
    // Elapsed units and hours/minutes are never touched by the profile.
    QCOMPARE(Duration::toMillis(1, Duration::ElapsedDays), 24LL * hour);
    QCOMPARE(Duration::toMillis(1, Duration::Hours), hour);

    // Zero / negative figures fall back to MS Project's defaults.
    Duration::setWorkingTimeProfile(0, 0, 0);
    QCOMPARE(Duration::toMillis(1, Duration::Days), 8LL * hour);

    Duration::resetWorkingTimeProfile();
    QCOMPARE(Duration::toMillis(1, Duration::Days), 8LL * hour);
}

void tst_duration::unitConversions()
{
    QCOMPARE(Duration::toMillis(1, Duration::Hours), 3600LL * 1000LL);
    QCOMPARE(Duration::toMillis(1, Duration::Days), 8LL * 3600LL * 1000LL);
    QCOMPARE(Duration::toMillis(1, Duration::Weeks), 40LL * 3600LL * 1000LL);
    QCOMPARE(Duration::toMillis(1, Duration::Months), 160LL * 3600LL * 1000LL);
    QCOMPARE(Duration::toMillis(1, Duration::ElapsedDays), 24LL * 3600LL * 1000LL);
    QCOMPARE(Duration::toMillis(1, Duration::ElapsedWeeks), 7LL * 24LL * 3600LL * 1000LL);
    QCOMPARE(Duration::fromMillis(4LL * 3600LL * 1000LL, Duration::Days), 0.5);
    QVERIFY(Duration::isElapsed(Duration::ElapsedHours));
    QVERIFY(!Duration::isElapsed(Duration::Hours));
}

void tst_duration::formatting_data()
{
    QTest::addColumn<qint64>("millis");
    QTest::addColumn<int>("unit");
    QTest::addColumn<QString>("expected");

    const qint64 hour = 3600LL * 1000LL;
    QTest::newRow("3 days")   << 3 * 8 * hour << int(Duration::Days) << "3 days";
    QTest::newRow("1 day")    << 8 * hour << int(Duration::Days) << "1 day";
    QTest::newRow("half day") << 4 * hour << int(Duration::Days) << "0.5 days";
    QTest::newRow("2.5 wks")  << qint64(2.5 * 40) * hour << int(Duration::Weeks) << "2.5 wks";
    QTest::newRow("1 wk")     << 40 * hour << int(Duration::Weeks) << "1 wk";
    QTest::newRow("4 hrs")    << 4 * hour << int(Duration::Hours) << "4 hrs";
    QTest::newRow("30 mins")  << hour / 2 << int(Duration::Minutes) << "30 mins";
    QTest::newRow("1 mon")    << 160 * hour << int(Duration::Months) << "1 mon";
    QTest::newRow("2 edays")  << 48 * hour << int(Duration::ElapsedDays) << "2 edays";
}

void tst_duration::formatting()
{
    QFETCH(qint64, millis);
    QFETCH(int, unit);
    QFETCH(QString, expected);
    QCOMPARE(Duration::format(millis, unit), expected);
}

void tst_duration::parsing_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<qint64>("millis");
    QTest::addColumn<int>("unit");

    const qint64 hour = 3600LL * 1000LL;
    QTest::newRow("bare number = days") << "3" << 3 * 8 * hour << int(Duration::Days);
    QTest::newRow("3d")        << "3d" << 3 * 8 * hour << int(Duration::Days);
    QTest::newRow("3 days")    << "3 days" << 3 * 8 * hour << int(Duration::Days);
    QTest::newRow("2 wks")     << "2 wks" << 80 * hour << int(Duration::Weeks);
    QTest::newRow("1.5w")      << "1.5w" << 60 * hour << int(Duration::Weeks);
    QTest::newRow("4 hrs")     << "4 hrs" << 4 * hour << int(Duration::Hours);
    QTest::newRow("4 hours")   << "4 hours" << 4 * hour << int(Duration::Hours);
    QTest::newRow("30 min")    << "30 min" << hour / 2 << int(Duration::Minutes);
    QTest::newRow("2 mo")      << "2 mo" << 320 * hour << int(Duration::Months);
    QTest::newRow("2 months")  << "2 months" << 320 * hour << int(Duration::Months);
    QTest::newRow("2 edays")   << "2 edays" << 48 * hour << int(Duration::ElapsedDays);
    QTest::newRow("1 ewk")     << "1 ewk" << 7 * 24 * hour << int(Duration::ElapsedWeeks);
    QTest::newRow("negative lead") << "-2d" << -16 * hour << int(Duration::Days);
    QTest::newRow("comma decimal") << "1,5d" << 12 * hour << int(Duration::Days);
    QTest::newRow("case insensitive") << "3 DAYS" << 24 * hour << int(Duration::Days);
}

void tst_duration::parsing()
{
    QFETCH(QString, text);
    QFETCH(qint64, millis);
    QFETCH(int, unit);
    qint64 outMs = -1;
    int outUnit = -1;
    QVERIFY(Duration::parse(text, &outMs, &outUnit));
    QCOMPARE(outMs, millis);
    QCOMPARE(outUnit, unit);
}

void tst_duration::parseRejects()
{
    qint64 ms = 0;
    int unit = 0;
    QVERIFY(!Duration::parse(QString(), &ms, &unit));
    QVERIFY(!Duration::parse(QStringLiteral("abc"), &ms, &unit));
    QVERIFY(!Duration::parse(QStringLiteral("3 parsecs"), &ms, &unit));
    QVERIFY(!Duration::parse(QStringLiteral("50%"), &ms, &unit));   // percent lag unsupported
}

void tst_duration::normalization()
{
    QCOMPARE(Duration::normalizeUnit(7), int(Duration::Days));
    QCOMPARE(Duration::normalizeUnit(39), int(Duration::Days));    // estimated days
    QCOMPARE(Duration::normalizeUnit(41), int(Duration::Weeks));   // estimated weeks
    QCOMPARE(Duration::normalizeUnit(21), int(Duration::Days));    // null -> default
    QCOMPARE(Duration::normalizeUnit(0), int(Duration::Days));
}

QTEST_APPLESS_MAIN(tst_duration)
#include "tst_duration.moc"
