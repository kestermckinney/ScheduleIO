// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

namespace {
constexpr quint32 kTaskName = 188743694u;
constexpr quint32 kTaskWork = 188743680u;
constexpr quint32 kResourceName = 205520897u;
constexpr quint32 kResourceWork = 205520909u;
}

class TstMsProjectUsageView : public QObject
{
    Q_OBJECT
private slots:
    void projectOpensInspectsAndResavesUsageSettings();
};

void TstMsProjectUsageView::projectOpensInspectsAndResavesUsageSettings()
{
    MppIO reader;
    const QString source = QStringLiteral(SCHEDULEIO_FIXTURE_DIR)
        + QStringLiteral("/Average Project.mpp");
    QVERIFY2(reader.open(source), qPrintable(reader.errorString()));
    schedule::Project edited = reader.project();
    edited.resourceUsageView.setColumnWidth(kResourceName, 31);
    edited.resourceUsageView.setColumnWidth(kResourceWork, 17);
    edited.resourceUsageView.setTimescaleSize(135);
    edited.resourceUsageView.setDetailFields({0, 67, 2, 5});
    edited.taskUsageView.setColumnWidth(kTaskName, 29);
    edited.taskUsageView.setColumnWidth(kTaskWork, 15);
    edited.taskUsageView.setTimescaleSize(145);
    edited.taskUsageView.setDetailFields({0, 72, 2, 5});

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString incoming = temp.filePath(QStringLiteral("usage-view-incoming.mpp"));
    const QString resaved = temp.filePath(QStringLiteral("usage-view-project-resaved.mpp"));
    MppIO writer;
    writer.setProject(edited);
    QVERIFY2(writer.save(incoming), qPrintable(writer.errorString()));

    QProcess projectOracle;
    projectOracle.setProgram(QStringLiteral("powershell.exe"));
    projectOracle.setArguments({QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
        QStringLiteral("-File"), QStringLiteral(SCHEDULEIO_USAGE_ORACLE_SCRIPT),
        QStringLiteral("-InputPath"), incoming,
        QStringLiteral("-OutputPath"), resaved});
    projectOracle.start();
    QVERIFY2(projectOracle.waitForStarted(10000), qPrintable(projectOracle.errorString()));
    QVERIFY2(projectOracle.waitForFinished(120000),
             "Microsoft Project usage-view oracle timed out");
    const QByteArray oracleOutput = projectOracle.readAllStandardOutput()
        + projectOracle.readAllStandardError();
    QCOMPARE(projectOracle.exitStatus(), QProcess::NormalExit);
    QVERIFY2(projectOracle.exitCode() == 0, oracleOutput.constData());

    MppIO projectResave;
    QVERIFY2(projectResave.open(resaved), qPrintable(projectResave.errorString()));
    const schedule::Project &actual = projectResave.project();
    QCOMPARE(actual.resourceUsageView.columnWidth(kResourceName), 31);
    QCOMPARE(actual.resourceUsageView.columnWidth(kResourceWork), 17);
    QCOMPARE(actual.resourceUsageView.timescaleSize, 135);
    QCOMPARE(actual.resourceUsageView.detailFields, QList<int>({0, 67, 2, 5}));
    QCOMPARE(actual.taskUsageView.columnWidth(kTaskName), 29);
    QCOMPARE(actual.taskUsageView.columnWidth(kTaskWork), 15);
    QCOMPARE(actual.taskUsageView.timescaleSize, 145);
    QCOMPARE(actual.taskUsageView.detailFields, QList<int>({0, 72, 2, 5}));
}

QTEST_APPLESS_MAIN(TstMsProjectUsageView)
#include "tst_msproject_usage_view.moc"
