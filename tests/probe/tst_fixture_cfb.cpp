// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "ole/compoundfile.h"

#include <QDir>
#include <QFile>
#include <QTest>

#ifndef MPPIO_FIXTURE_DIR
#define MPPIO_FIXTURE_DIR ""
#endif

// Validates the real MS-CFB reader against the user's real .mpp fixtures, and
// prints each file's storage tree. This is how we learn the actual internal
// stream/storage names to drive the entity-mapping work (plan, Layers 2-3).
class TstFixtureCfb : public QObject
{
    Q_OBJECT
private slots:
    void parsesRealContainers_data();
    void parsesRealContainers();

private:
    void dump(const CompoundFile &cf, const QStringList &path, int depth, int maxDepth);
};

void TstFixtureCfb::dump(const CompoundFile &cf, const QStringList &path, int depth, int maxDepth)
{
    const QStringList kids = cf.childNames(path);
    for (const QString &name : kids) {
        QStringList child = path;
        child << name;
        const bool isStorage = cf.hasStorage(child);
        const QString indent(depth * 2, QChar(' '));
        if (isStorage) {
            qInfo().noquote() << indent + "[" + name + "]";
            if (depth < maxDepth)
                dump(cf, child, depth + 1, maxDepth);
        } else {
            qInfo().noquote() << indent + name
                              + QStringLiteral("  (%1 bytes)").arg(cf.readStream(child).size());
        }
    }
}

void TstFixtureCfb::parsesRealContainers_data()
{
    QTest::addColumn<QString>("path");
    const QString dir = QStringLiteral(MPPIO_FIXTURE_DIR);
    for (const QString &f : QDir(dir).entryList({ QStringLiteral("*.mpp") }, QDir::Files))
        QTest::newRow(qPrintable(f)) << QDir(dir).filePath(f);
}

void TstFixtureCfb::parsesRealContainers()
{
    if (QDir(QStringLiteral(MPPIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files)
            .isEmpty())
        QSKIP("no .mpp fixtures present");

    QFETCH(QString, path);

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();

    // A real .mpp must look like a compound file and parse without error.
    QVERIFY(CompoundFile::looksLikeCompoundFile(bytes));

    CompoundFile cf;
    QVERIFY2(cf.openFromData(bytes), qPrintable(cf.errorString()));

    qInfo().noquote() << "==== storage tree of" << QFileInfo(path).fileName() << "====";
    dump(cf, {}, 0, 2);

    // Real .mpp files carry a top-level "Props<NN>" version-marker stream.
    const QStringList top = cf.childNames({});
    const bool hasVersionStream =
        top.contains(QStringLiteral("Props14")) || top.contains(QStringLiteral("Props12"));
    QVERIFY2(hasVersionStream, qPrintable(QStringLiteral("no Props12/Props14 marker; top-level: %1")
                                              .arg(top.join(QStringLiteral(", ")))));
    qInfo().noquote() << "detected version stream present; top-level entries:" << top.join(", ");
}

QTEST_MAIN(TstFixtureCfb)
#include "tst_fixture_cfb.moc"
