// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/propsreader.h"
#include "ole/compoundfile.h"

#include <QDir>
#include <QFile>
#include <QTest>
#include <QtEndian>

#ifndef MPPIO_FIXTURE_DIR
#define MPPIO_FIXTURE_DIR ""
#endif

class TstPropsReader : public QObject
{
    Q_OBJECT
private slots:
    void parsesSyntheticProps();
    void rejectsTooShort();
    void readsEntityNameListFromFixtures_data();
    void readsEntityNameListFromFixtures();
};

// Build a minimal Props blob: 16-byte header + items [len][key][flags][data].
static QByteArray makeProps(const QList<QPair<quint32, QByteArray>> &items)
{
    QByteArray b(16, '\0');
    for (const auto &it : items) {
        QByteArray h(12, '\0');
        qToLittleEndian<quint32>(quint32(it.second.size()), reinterpret_cast<uchar *>(h.data()));
        qToLittleEndian<quint32>(it.first, reinterpret_cast<uchar *>(h.data() + 4));
        b.append(h);
        b.append(it.second);
    }
    qToLittleEndian<quint32>(quint32(b.size()), reinterpret_cast<uchar *>(b.data()));
    return b;
}

void TstPropsReader::parsesSyntheticProps()
{
    const QByteArray blob = makeProps({
        { 0x02400001u, QByteArray("\x2a\x00\x00\x00", 4) },     // an int
        { 0x024003e8u, QByteArray("\x41\x00\x42\x00", 4) },     // "AB" UTF-16
    });
    PropsReader p;
    QVERIFY(p.parse(blob));
    QCOMPARE(p.count(), 2);
    QVERIFY(p.contains(0x02400001u));
    QCOMPARE(p.value(0x02400001u), QByteArray("\x2a\x00\x00\x00", 4));
    QCOMPARE(p.string(0x024003e8u), QStringLiteral("AB"));
    QVERIFY(!p.contains(0x12345678u));
}

void TstPropsReader::rejectsTooShort()
{
    PropsReader p;
    QVERIFY(!p.parse(QByteArray(8, '\0')));
    QVERIFY(!p.parse(QByteArray()));
}

void TstPropsReader::readsEntityNameListFromFixtures_data()
{
    QTest::addColumn<QString>("path");
    const QString dir = QStringLiteral(MPPIO_FIXTURE_DIR);
    for (const QString &f : QDir(dir).entryList({ QStringLiteral("*.mpp") }, QDir::Files))
        QTest::newRow(qPrintable(f)) << QDir(dir).filePath(f);
}

void TstPropsReader::readsEntityNameListFromFixtures()
{
    if (QDir(QStringLiteral(MPPIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files)
            .isEmpty())
        QSKIP("no .mpp fixtures present");

    QFETCH(QString, path);
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    CompoundFile cf;
    QVERIFY(cf.openFromData(f.readAll()));

    PropsReader props;
    QVERIFY(props.parse(cf.readStream({ QStringLiteral("   114"), QStringLiteral("Props") })));

    // Key 0x024003e8 holds the entity-name list; it must name the core entities.
    const QString entities = props.string(0x024003e8u);
    QVERIFY2(entities.contains(QStringLiteral("TBkndTask")),
             qPrintable(QStringLiteral("entity list was: '%1'").arg(entities)));
    QVERIFY(entities.contains(QStringLiteral("TBkndRsc")));
}

QTEST_APPLESS_MAIN(TstPropsReader)
#include "tst_propsreader.moc"
