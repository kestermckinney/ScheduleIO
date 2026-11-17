// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// The named filter and group a view has applied (Project's Filter and Group
// boxes), against files Microsoft Project 2016 wrote with FilterApply and
// GroupApply (tests/fixtures/mpp_samples/viewfilter and viewgroup, made by
// ScheduleVault's tools/viewfilter-samples-remote.vbs and
// viewgroup-samples-remote.vbs). Project keeps the filter's name in the view's
// Props9 item 0x2240000B and copies its definition into the file's CFilter
// rowset (it resolves a built-in name from its global template when the file
// has none); the group is its name (0x22400018) and an 83-byte copy of its
// definition (0x2240001D), both in the view.

#include "mppio.h"
#include "ole/compoundfile.h"

#include <QFile>
#include <QTest>
#include <QtEndian>

namespace {

const QString kDir = QStringLiteral(SCHEDULEIO_FIXTURE_DIR "/mpp_samples/viewfilter/");
const QString kGroupDir = QStringLiteral(SCHEDULEIO_FIXTURE_DIR "/mpp_samples/viewgroup/");

QByteArray fixture(const QString &name)
{
    QFile f(name.startsWith(QLatin1String("vg-")) ? kGroupDir + name : kDir + name);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = f.readAll();
    return bytes.startsWith("version https://git-lfs") ? QByteArray() : bytes;
}

// The names of the records in one of the file's 230-byte presentation rowsets
// (CFilter, CGrouping), in uid order.
QStringList definedRecords(const QByteArray &file, const QString &rowset)
{
    QStringList names;
    CompoundFile cf;
    if (!cf.openFromData(file))
        return names;
    const QByteArray data = cf.readStream({ QStringLiteral("   214"), rowset,
                                            QStringLiteral("FixedData") });
    for (int o = 0; o + 230 <= data.size(); o += 230) {
        QString name;
        for (int i = o + 4; i + 1 < o + 110; i += 2) {
            const ushort ch = ushort(uchar(data[i]) | (uchar(data[i + 1]) << 8));
            if (ch == 0)
                break;
            name.append(QChar(ch));
        }
        names.append(name);
    }
    return names;
}

QStringList definedFilters(const QByteArray &file)
{
    return definedRecords(file, QStringLiteral("CFilter"));
}

} // namespace

class TstViewFilter : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void readsTheAppliedFilter();
    void plainRoundTripKeepsTheFile();
    void writesANewFilter();
    void newProjectCarriesTheFilter();
    void readsEveryViewsFilter();
    void groupDefinitionsAreProjects_data();
    void groupDefinitionsAreProjects();
    void writesFiltersOnViewsTheFileLacks();
    void writesAndClearsAGroup();
    void newProjectCarriesViewFiltersAndGroup();
};

void TstViewFilter::initTestCase()
{
    if (fixture(QStringLiteral("vf-all.mpp")).isEmpty())
        QSKIP("view filter fixtures missing (run git lfs pull)");
}

void TstViewFilter::readsTheAppliedFilter()
{
    const struct { const char *file; const char *gantt; const char *usage; } cases[] = {
        { "vf-all.mpp", "All Tasks", "" },
        { "vf-critical.mpp", "Critical", "" },
        { "vf-milestones.mpp", "Milestones", "" },
        { "vf-summary.mpp", "Summary Tasks", "" },
        { "vf-tu-milestones.mpp", "&All Tasks", "Milestones" },
    };
    for (const auto &c : cases) {
        MppIO io;
        QVERIFY2(io.openFromData(fixture(QString::fromLatin1(c.file))), c.file);
        const schedule::Project p = io.project();
        QCOMPARE(p.ganttView.filterName, QString::fromLatin1(c.gantt));
        QCOMPARE(p.taskUsageView.filterName, QString::fromLatin1(c.usage));
    }
}

void TstViewFilter::plainRoundTripKeepsTheFile()
{
    MppIO io;
    QVERIFY(io.openFromData(fixture(QStringLiteral("vf-critical.mpp"))));
    const QByteArray saved = io.saveToData();
    QVERIFY2(!saved.isEmpty(), qPrintable(io.errorString()));
    MppIO back;
    QVERIFY(back.openFromData(saved));
    QCOMPARE(back.project().ganttView.filterName, QStringLiteral("Critical"));
    // The file's own definition goes with the view that names it.
    QVERIFY(definedFilters(saved).contains(QStringLiteral("&Critical")));
}

void TstViewFilter::writesANewFilter()
{
    MppIO io;
    QVERIFY(io.openFromData(fixture(QStringLiteral("vf-critical.mpp"))));
    schedule::Project p = io.project();
    p.ganttView.setFilterName(QStringLiteral("Incomplete Tasks"));
    io.setProject(p);
    const QByteArray saved = io.saveToData();
    QVERIFY2(!saved.isEmpty(), qPrintable(io.errorString()));
    MppIO back;
    QVERIFY(back.openFromData(saved));
    const schedule::Project q = back.project();
    QCOMPARE(q.ganttView.filterName, QStringLiteral("Incomplete Tasks"));
    QCOMPARE(q.ganttView.tableName, QStringLiteral("&Entry"));
    QCOMPARE(q.tasks.size(), p.tasks.size());

    // Back to All Tasks: the hotkey spelling the file used is not rewritten.
    p = q;
    p.ganttView.setFilterName(QStringLiteral("All Tasks"));
    io.setProject(p);
    MppIO again;
    QVERIFY(again.openFromData(io.saveToData()));
    QCOMPARE(again.project().ganttView.filterName, QStringLiteral("All Tasks"));
}

void TstViewFilter::newProjectCarriesTheFilter()
{
    schedule::Project p;
    p.title = QStringLiteral("Filtered");
    schedule::Task t;
    t.uniqueId = 1;
    t.id = 1;
    t.name = QStringLiteral("Only");
    t.outlineLevel = 1;
    p.tasks.append(t);
    p.ganttView.setFilterName(QStringLiteral("Milestones"));
    MppIO io;
    io.setProject(p);
    const QByteArray saved = io.saveToData();
    QVERIFY2(!saved.isEmpty(), qPrintable(io.errorString()));
    MppIO back;
    QVERIFY(back.openFromData(saved));
    QCOMPARE(back.project().ganttView.filterName, QStringLiteral("Milestones"));

    // Left unset, the template's own filter stays.
    schedule::Project plain = p;
    plain.ganttView = {};
    io.setProject(plain);
    MppIO stock;
    QVERIFY(stock.openFromData(io.saveToData()));
    QCOMPARE(stock.project().ganttView.filterName, QStringLiteral("&All Tasks"));
}

void TstViewFilter::readsEveryViewsFilter()
{
    MppIO io;
    QVERIFY(io.openFromData(fixture(QStringLiteral("vg-views-filtered.mpp"))));
    const schedule::Project p = io.project();
    QCOMPARE(p.ganttView.filterName, QStringLiteral("&All Tasks"));
    QCOMPARE(p.taskUsageView.filterName, QStringLiteral("Milestones"));
    QCOMPARE(p.calendarView.filterName, QStringLiteral("Critical"));
    QCOMPARE(p.networkView.filterName, QStringLiteral("Incomplete Tasks"));
    QCOMPARE(p.resourceUsageView.filterName, QStringLiteral("Overallocated Resources"));
    QCOMPARE(p.teamPlannerView.filterName, QStringLiteral("Resources - Work"));
    QCOMPARE(p.resourceSheetView.filterName, QStringLiteral("Resources - Material"));
}

void TstViewFilter::groupDefinitionsAreProjects_data()
{
    QTest::addColumn<QString>("file");
    QTest::addColumn<bool>("taskUsage");
    QTest::addColumn<QString>("group");
    QTest::newRow("none") << "vg-base.mpp" << false << "No Group";
    QTest::newRow("critical") << "vg-group-critical.mpp" << false << "Critical";
    QTest::newRow("status") << "vg-group-status.mpp" << false << "Status";
    QTest::newRow("complete") << "vg-group-complete.mpp" << false << "Complete and Incomplete Tasks";
    QTest::newRow("mode") << "vg-group-mode.mpp" << false << "Auto Scheduled v. Manually Scheduled";
    QTest::newRow("outline") << "vg-group-outline.mpp" << false << "Outline Level";
    QTest::newRow("cleared") << "vg-group-none.mpp" << false << "No Group";
    QTest::newRow("task usage") << "vg-tu-group-critical.mpp" << true << "Critical";
}

void TstViewFilter::groupDefinitionsAreProjects()
{
    QFETCH(QString, file);
    QFETCH(bool, taskUsage);
    QFETCH(QString, group);
    MppIO io;
    QVERIFY(io.openFromData(fixture(file)));
    const schedule::UsageViewSettings &v =
        taskUsage ? io.project().taskUsageView : io.project().ganttView;
    QCOMPARE(v.groupName, group);
    QCOMPARE(v.groupDefinition.size(), 83);
    QCOMPARE(schedule::projectGroupDefinition(group).toHex(), v.groupDefinition.toHex());
}

void TstViewFilter::writesFiltersOnViewsTheFileLacks()
{
    MppIO io;
    QVERIFY(io.openFromData(fixture(QStringLiteral("vg-base.mpp"))));
    schedule::Project p = io.project();
    QVERIFY(!p.taskUsageView.present);
    p.taskUsageView.setFilterName(QStringLiteral("Milestones"));
    p.calendarView.setFilterName(QStringLiteral("Critical"));
    p.networkView.setFilterName(QStringLiteral("Late Tasks"));
    p.resourceUsageView.setFilterName(QStringLiteral("Overallocated Resources"));
    p.teamPlannerView.setFilterName(QStringLiteral("Resources - Work"));
    p.resourceSheetView.setFilterName(QStringLiteral("Resources - Cost"));
    io.setProject(p);
    const QByteArray saved = io.saveToData();
    QVERIFY2(!saved.isEmpty(), qPrintable(io.errorString()));
    MppIO back;
    QVERIFY(back.openFromData(saved));
    const schedule::Project q = back.project();
    QCOMPARE(q.ganttView.filterName, QStringLiteral("&All Tasks"));
    QCOMPARE(q.taskUsageView.filterName, QStringLiteral("Milestones"));
    QCOMPARE(q.taskUsageView.tableName, QStringLiteral("Usa&ge"));
    QCOMPARE(q.calendarView.filterName, QStringLiteral("Critical"));
    QCOMPARE(q.networkView.filterName, QStringLiteral("Late Tasks"));
    QCOMPARE(q.resourceUsageView.filterName, QStringLiteral("Overallocated Resources"));
    QCOMPARE(q.teamPlannerView.filterName, QStringLiteral("Resources - Work"));
    QCOMPARE(q.resourceSheetView.filterName, QStringLiteral("Resources - Cost"));
    QCOMPARE(q.tasks.size(), p.tasks.size());

    // An everything-filter needs no view: none is added for it.
    schedule::Project plain = io.project();
    plain.calendarView.setFilterName(QStringLiteral("All Tasks"));
    MppIO none;
    QVERIFY(none.openFromData(fixture(QStringLiteral("vg-base.mpp"))));
    none.setProject(plain);
    MppIO plainBack;
    QVERIFY(plainBack.openFromData(none.saveToData()));
    QVERIFY(plainBack.project().calendarView.filterName.isEmpty());
}

void TstViewFilter::writesAndClearsAGroup()
{
    MppIO io;
    QVERIFY(io.openFromData(fixture(QStringLiteral("vg-base.mpp"))));
    schedule::Project p = io.project();
    QVERIFY(p.ganttView.setGroup(QStringLiteral("Outline Level")));
    QVERIFY(!p.ganttView.setGroup(QStringLiteral("Not a group")));
    QVERIFY(p.taskUsageView.setGroup(QStringLiteral("Critical")));
    io.setProject(p);
    const QByteArray saved = io.saveToData();
    QVERIFY2(!saved.isEmpty(), qPrintable(io.errorString()));
    MppIO back;
    QVERIFY(back.openFromData(saved));
    schedule::Project q = back.project();
    QCOMPARE(q.ganttView.groupName, QStringLiteral("Outline Level"));
    QCOMPARE(q.taskUsageView.groupName, QStringLiteral("Critical"));
    // Each group's record comes into CGrouping (a custom group shows only with
    // it), and the view's definition starts with that record's uid; the rest
    // is Project's bytes.
    const QStringList groups = definedRecords(saved, QStringLiteral("CGrouping"));
    QCOMPARE(groups, (QStringList{ QStringLiteral("&No Group"), QStringLiteral("&No Group"),
                                   QStringLiteral("Outline Level"), QStringLiteral("C&ritical") }));
    QCOMPARE(qFromLittleEndian<quint32>(q.ganttView.groupDefinition.constData()), 3u);
    QCOMPARE(qFromLittleEndian<quint32>(q.taskUsageView.groupDefinition.constData()), 4u);
    QCOMPARE(q.ganttView.groupDefinition.mid(4),
             schedule::projectGroupDefinition(QStringLiteral("Outline Level")).mid(4));

    QVERIFY(q.ganttView.setGroup(QStringLiteral("No Group")));
    back.setProject(q);
    MppIO cleared;
    QVERIFY(cleared.openFromData(back.saveToData()));
    QCOMPARE(cleared.project().ganttView.groupName, QStringLiteral("No Group"));
    QCOMPARE(cleared.project().ganttView.groupDefinition,
             schedule::projectGroupDefinition(QStringLiteral("No Group")));
    // A file's own group definitions stay with it.
    QVERIFY(definedRecords(back.saveToData(), QStringLiteral("CGrouping"))
                .contains(QStringLiteral("Outline Level")));
}

void TstViewFilter::newProjectCarriesViewFiltersAndGroup()
{
    schedule::Project p;
    p.title = QStringLiteral("Filtered");
    schedule::Task t;
    t.uniqueId = 1;
    t.id = 1;
    t.name = QStringLiteral("Only");
    t.outlineLevel = 1;
    p.tasks.append(t);
    p.networkView.setFilterName(QStringLiteral("Milestones"));
    p.teamPlannerView.setFilterName(QStringLiteral("Overallocated Resources"));
    QVERIFY(p.ganttView.setGroup(QStringLiteral("Critical")));
    MppIO io;
    io.setProject(p);
    const QByteArray saved = io.saveToData();
    QVERIFY2(!saved.isEmpty(), qPrintable(io.errorString()));
    MppIO back;
    QVERIFY(back.openFromData(saved));
    QCOMPARE(back.project().networkView.filterName, QStringLiteral("Milestones"));
    QCOMPARE(back.project().teamPlannerView.filterName, QStringLiteral("Overallocated Resources"));
    QCOMPARE(back.project().ganttView.groupName, QStringLiteral("Critical"));
}

QTEST_GUILESS_MAIN(TstViewFilter)
#include "tst_viewfilter.moc"
