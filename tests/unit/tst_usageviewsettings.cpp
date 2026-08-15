// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"
#include "ole/compoundfile.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {
constexpr quint32 kTaskName = 188743694u;
constexpr quint32 kTaskWork = 188743680u;
constexpr quint32 kResourceName = 205520897u;
constexpr quint32 kResourceWork = 205520909u;
}

class TstUsageViewSettings : public QObject
{
    Q_OBJECT
private slots:
    void readsNativeUsageTables();
    void writesNativeWidthsAndTimescale();
    void preservesSourceTableRowsetWhenUnchanged();
};

static QString fixture(const QString &name)
{
    return QStringLiteral(SCHEDULEIO_FIXTURE_DIR) + QLatin1Char('/') + name;
}

void TstUsageViewSettings::readsNativeUsageTables()
{
    MppIO io;
    QVERIFY2(io.open(fixture(QStringLiteral("Average Project.mpp"))),
             qPrintable(io.errorString()));
    const schedule::Project &project = io.project();
    QVERIFY(project.resourceUsageView.present);
    QVERIFY(project.taskUsageView.present);
    QVERIFY(project.ganttView.present);
    QCOMPARE(QString(project.resourceUsageView.tableName).remove(QLatin1Char('&')),
             QStringLiteral("Usage"));
    QCOMPARE(QString(project.taskUsageView.tableName).remove(QLatin1Char('&')),
             QStringLiteral("Usage"));
    QVERIFY(project.resourceUsageView.columnWidth(kResourceName) > 0);
    QVERIFY(project.resourceUsageView.columnWidth(kResourceWork) > 0);
    QVERIFY(project.taskUsageView.columnWidth(kTaskName) > 0);
    QVERIFY(project.taskUsageView.columnWidth(kTaskWork) > 0);
    QVERIFY(project.resourceUsageView.timescaleSize >= 25);
    QVERIFY(project.taskUsageView.timescaleSize >= 25);
    QCOMPARE(project.taskUsageView.detailFields, QList<int>({0, 72}));
    QVERIFY(project.ganttView.tableWidth > 0);
}

void TstUsageViewSettings::writesNativeWidthsAndTimescale()
{
    MppIO reader;
    QVERIFY2(reader.open(fixture(QStringLiteral("Average Project.mpp"))),
             qPrintable(reader.errorString()));
    schedule::Project edited = reader.project();
    edited.resourceUsageView.setColumnWidth(kResourceName, 31);
    edited.resourceUsageView.setColumnWidth(kResourceWork, 17);
    edited.resourceUsageView.setTimescaleSize(135);
    edited.resourceUsageView.setTableWidth(406);
    edited.resourceUsageView.setDetailFields({0, 67, 2, 5});
    edited.resourceUsageView.setDetailSelection({QStringLiteral("Work"),
        QStringLiteral("Actual Work"), QStringLiteral("Baseline Work")});
    edited.taskUsageView.setColumnWidth(kTaskName, 29);
    edited.taskUsageView.setColumnWidth(kTaskWork, 15);
    edited.taskUsageView.setTimescaleSize(145);
    edited.taskUsageView.setTableWidth(398);
    edited.taskUsageView.setDetailFields({0, 72, 2, 5});
    edited.taskUsageView.setDetailSelection({QStringLiteral("Work"),
        QStringLiteral("Remaining Work"), QStringLiteral("Remaining Cost")});
    edited.ganttView.setColumnWidth(kTaskName, 37);
    edited.ganttView.setTableWidth(512);

    MppIO writer;
    writer.setProject(edited);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));

    MppIO roundTrip;
    QVERIFY2(roundTrip.openFromData(bytes), qPrintable(roundTrip.errorString()));
    const schedule::Project &actual = roundTrip.project();
    QCOMPARE(actual.resourceUsageView.columnWidth(kResourceName), 31);
    QCOMPARE(actual.resourceUsageView.columnWidth(kResourceWork), 17);
    QCOMPARE(actual.resourceUsageView.timescaleSize, 135);
    QCOMPARE(actual.resourceUsageView.tableWidth, 406);
    QCOMPARE(actual.resourceUsageView.detailFields, QList<int>({0, 67, 2, 5}));
    QCOMPARE(actual.resourceUsageView.detailSelection,
             QStringList({QStringLiteral("Work"), QStringLiteral("Actual Work"),
                          QStringLiteral("Baseline Work")}));
    QCOMPARE(actual.taskUsageView.columnWidth(kTaskName), 29);
    QCOMPARE(actual.taskUsageView.columnWidth(kTaskWork), 15);
    QCOMPARE(actual.taskUsageView.timescaleSize, 145);
    QCOMPARE(actual.taskUsageView.tableWidth, 398);
    QCOMPARE(actual.taskUsageView.detailFields, QList<int>({0, 72, 2, 5}));
    QCOMPARE(actual.taskUsageView.detailSelection,
             QStringList({QStringLiteral("Work"), QStringLiteral("Remaining Work"),
                          QStringLiteral("Remaining Cost")}));
    QCOMPARE(actual.ganttView.columnWidth(kTaskName), 37);
    QCOMPARE(actual.ganttView.tableWidth, 512);
}

void TstUsageViewSettings::preservesSourceTableRowsetWhenUnchanged()
{
    QFile file(fixture(QStringLiteral("Average Project.mpp")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray sourceBytes = file.readAll();
    MppIO reader;
    QVERIFY2(reader.openFromData(sourceBytes), qPrintable(reader.errorString()));
    MppIO writer;
    writer.setProject(reader.project());
    const QByteArray savedBytes = writer.saveToData();
    QVERIFY2(!savedBytes.isEmpty(), qPrintable(writer.errorString()));

    CompoundFile source, saved;
    QVERIFY(source.openFromData(sourceBytes));
    QVERIFY(saved.openFromData(savedBytes));
    const QStringList base{QStringLiteral("   214"), QStringLiteral("CTable")};
    for (const QString &stream : {QStringLiteral("FixedMeta"), QStringLiteral("FixedData"),
                                  QStringLiteral("VarMeta"), QStringLiteral("Var2Data")})
        QCOMPARE(saved.readStream(base + QStringList{stream}),
                 source.readStream(base + QStringList{stream}));
}

QTEST_APPLESS_MAIN(TstUsageViewSettings)
#include "tst_usageviewsettings.moc"
