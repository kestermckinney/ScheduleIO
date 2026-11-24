// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// "   214/CEdl", Project's saved window state. The embedded template carries an
// empty cached row list (item 0x26400035) and row table (0x2640003d); Project
// trusted those over the task table and opened a file with a real outline to a
// blank Gantt grid (Big Test.mpp, 11,568 tasks, 2026-11-19). The writer drops
// both items so Project lays the rows out itself.

#include "mppio.h"
#include "ole/compoundfile.h"

#include <QTest>
#include <QtEndian>

namespace {

quint32 u32(const QByteArray &b, int off)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + off));
}

schedule::Task task(int uid, int level, const QString &name)
{
    schedule::Task t;
    t.uniqueId = uid;
    t.id = uid;
    t.outlineLevel = level;
    t.name = name;
    return t;
}

} // namespace

class TstWindowState : public QObject
{
    Q_OBJECT

private slots:
    void savedWindowStateHasNoCachedRows();
};

void TstWindowState::savedWindowStateHasNoCachedRows()
{
    schedule::Project p;
    p.title = QStringLiteral("Outline");
    p.tasks.append(task(1, 1, QStringLiteral("Phase")));
    p.tasks.append(task(2, 2, QStringLiteral("Work")));
    p.tasks.append(task(3, 2, QStringLiteral("More work")));
    MppIO io;
    io.setProject(p);
    const QByteArray saved = io.saveToData();
    QVERIFY2(!saved.isEmpty(), qPrintable(io.errorString()));

    CompoundFile cf;
    QVERIFY(cf.openFromData(saved));
    const QStringList cedl = { QStringLiteral("   214"), QStringLiteral("CEdl") };
    const QByteArray blob = cf.readStream(cedl + QStringList{ QStringLiteral("Var2Data") });
    const QByteArray meta = cf.readStream(cedl + QStringList{ QStringLiteral("VarMeta") });
    QVERIFY(blob.size() > 20);
    QCOMPARE(u32(meta, 20), quint32(blob.size()));
    QCOMPARE(u32(blob, 0), quint32(blob.size() - 4));
    QCOMPARE(u32(blob, 4), quint32(blob.size() - 8));
    QCOMPARE(u32(blob, 8), quint32(blob.size() - 8));

    QList<quint32> keys;
    int off = 20;
    while (off + 12 <= blob.size()) {
        const quint32 len = u32(blob, off);
        keys.append(u32(blob, off + 4));
        QCOMPARE(u32(blob, off + 8), 1u);
        off += int(12 + len);
    }
    QCOMPARE(off, int(blob.size()));
    QCOMPARE(u32(blob, 16), quint32(keys.size()));
    QVERIFY(keys.contains(0x26400018u));   // the view name survives
    QVERIFY(!keys.contains(0x26400035u));
    QVERIFY(!keys.contains(0x2640003Du));

    MppIO back;
    QVERIFY(back.openFromData(saved));
    QCOMPARE(back.project().tasks.size() >= 3, true);
}

QTEST_GUILESS_MAIN(TstWindowState)
#include "tst_windowstate.moc"
