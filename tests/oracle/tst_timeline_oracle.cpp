// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"
#include "model/project.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTest>

#include "fixtureutils.h"

// Layer 3 oracle for the Microsoft Project Timeline view. The MSPDI .xml export
// carries none of the timeline state, so each `mpp_samples/tl_*` folder's
// manifest.json (written by generate_timeline_samples.py from the intended COM
// operations) is the oracle. Asserts that ViewFormat::read decoded the
// `<TLViewData>` document into Project::timelineView.
class TstTimelineOracle : public QObject
{
    Q_OBJECT
private slots:
    void timelineMatchesManifest_data();
    void timelineMatchesManifest();
    void richFixtureDecodes();
    void membershipEditRoundTrips();
    void calloutDecodesFromRealMsp();
    void calloutEditRoundTrips();
    void propsEditRoundTrips();
    void barAssignmentRoundTrips();
    void untouchedWhenOtherViewPatched();
    void noOpEditIsStable_data();
    void noOpEditIsStable();

private:
    static QString fixture(const QString &fileName);
    static schedule::Project reload(const schedule::Project &edited);
};

QString TstTimelineOracle::fixture(const QString &fileName)
{
    for (const QString &mpp : fixtures::mppFiles())
        if (QFileInfo(mpp).fileName() == fileName)
            return mpp;
    return {};
}

// Match ScheduleVault's save path: a fresh writer, setProject(edited), reload.
schedule::Project TstTimelineOracle::reload(const schedule::Project &edited)
{
    MppIO writer;
    writer.setProject(edited);
    const QByteArray bytes = writer.saveToData();
    [&] { QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString())); }();
    MppIO reader;
    [&] { QVERIFY2(reader.openFromData(bytes), qPrintable(reader.errorString())); }();
    return reader.project();
}

void TstTimelineOracle::membershipEditRoundTrips()
{
    const QString path = fixture(QStringLiteral("tl_02_many_bar_tasks.mpp"));
    if (path.isEmpty())
        QSKIP("tl_02 fixture not installed");
    MppIO io;
    QVERIFY2(io.open(path), qPrintable(io.errorString()));
    schedule::Project p = io.project();

    QSet<int> before;
    for (const schedule::TimelineItem &it : p.timelineView.items)
        before.insert(it.taskUid);
    QVERIFY(before.contains(3));
    QVERIFY(!before.contains(4));

    // Drop uid 3, add uid 4 on bar 1.
    for (int i = p.timelineView.items.size() - 1; i >= 0; --i)
        if (p.timelineView.items[i].taskUid == 3)
            p.timelineView.items.removeAt(i);
    schedule::TimelineItem add;
    add.guid = QStringLiteral("{11111111-2222-3333-4444-555555555555}");
    add.taskUid = 4;
    add.barId = 1;
    p.timelineView.touch();
    p.timelineView.items.append(add);

    const schedule::Project rp = reload(p);
    QVERIFY2(rp.timelineView.present, "timeline lost on save");
    QSet<int> after;
    for (const schedule::TimelineItem &it : rp.timelineView.items)
        after.insert(it.taskUid);
    QVERIFY2(!after.contains(3), "removed member came back");
    QVERIFY2(after.contains(4), "added member missing");
    QCOMPARE(after, (before - QSet<int>{3}) | QSet<int>{4});
}

// tl_16_callout.mpp was produced on real MS Project 2016: uid 1 stays a bar,
// uid 2 was switched to "Display as Callout". Confirms our reader recognises
// MS Project's own callout encoding (<t onTL="0"> + an <ft> row in <fltSet>).
void TstTimelineOracle::calloutDecodesFromRealMsp()
{
    const QString path = fixture(QStringLiteral("tl_16_callout.mpp"));
    if (path.isEmpty())
        QSKIP("tl_16 fixture not installed");
    MppIO io;
    QVERIFY2(io.open(path), qPrintable(io.errorString()));
    const schedule::Project p = io.project();
    QVERIFY(p.timelineView.present);

    schedule::TimelineItemDisplay d1{}, d2{};
    bool s1 = false, s2 = false;
    for (const schedule::TimelineItem &it : p.timelineView.items) {
        QVERIFY2(it.onTimeline, "callout member must still read as on-timeline");
        if (it.taskUid == 1) { d1 = it.display; s1 = true; }
        if (it.taskUid == 2) { d2 = it.display; s2 = true; }
    }
    QVERIFY2(s1 && s2, "expected timeline members uid 1 and 2");
    QCOMPARE(d1, schedule::TimelineItemDisplay::Bar);
    QCOMPARE(d2, schedule::TimelineItemDisplay::Callout);
}

// A bar->callout edit (and back) must survive our own save/reload, and the
// emitted <TLViewData> must carry MS Project's callout encoding.
void TstTimelineOracle::calloutEditRoundTrips()
{
    const QString path = fixture(QStringLiteral("tl_02_many_bar_tasks.mpp"));
    if (path.isEmpty())
        QSKIP("tl_02 fixture not installed");
    MppIO io;
    QVERIFY2(io.open(path), qPrintable(io.errorString()));
    schedule::Project p = io.project();

    int target = -1;
    for (const schedule::TimelineItem &it : p.timelineView.items) {
        QCOMPARE(it.display, schedule::TimelineItemDisplay::Bar);
        if (target < 0)
            target = it.taskUid;
    }
    QVERIFY(target > 0);

    // bar -> callout
    for (schedule::TimelineItem &it : p.timelineView.items)
        if (it.taskUid == target)
            it.display = schedule::TimelineItemDisplay::Callout;
    p.timelineView.touch();

    schedule::Project rp = reload(p);
    QVERIFY(rp.timelineView.present);
    const QString xml = QString::fromUtf16(
        reinterpret_cast<const char16_t *>(rp.timelineView.rawXml.constData()),
        rp.timelineView.rawXml.size() / 2);
    QVERIFY2(xml.contains(QStringLiteral("<ft id=")), "no <ft> callout row emitted");
    QVERIFY2(xml.contains(QStringLiteral("uid=\"%1\" onTL=\"1\" top=\"1\"").arg(target)),
             "callout <ft> row missing expected attributes");
    QVERIFY2(xml.contains(QStringLiteral("<t id=\"") )
                 && xml.contains(QStringLiteral("uid=\"%1\" onTL=\"0\"").arg(target)),
             "callout <t> row should carry onTL=\"0\"");
    bool seenCallout = false;
    for (const schedule::TimelineItem &it : rp.timelineView.items) {
        if (it.taskUid == target) {
            QCOMPARE(it.display, schedule::TimelineItemDisplay::Callout);
            QVERIFY(it.onTimeline);
            seenCallout = true;
        } else {
            QCOMPARE(it.display, schedule::TimelineItemDisplay::Bar);
        }
    }
    QVERIFY2(seenCallout, "callout member lost after reload");

    // callout -> bar
    for (schedule::TimelineItem &it : rp.timelineView.items)
        if (it.taskUid == target)
            it.display = schedule::TimelineItemDisplay::Bar;
    rp.timelineView.touch();

    const schedule::Project rp2 = reload(rp);
    const QString xml2 = QString::fromUtf16(
        reinterpret_cast<const char16_t *>(rp2.timelineView.rawXml.constData()),
        rp2.timelineView.rawXml.size() / 2);
    QVERIFY2(!xml2.contains(QStringLiteral("uid=\"%1\" onTL=\"1\" top=\"1\"").arg(target)),
             "callout <ft> row should be gone after reverting to bar");
    for (const schedule::TimelineItem &it : rp2.timelineView.items)
        QCOMPARE(it.display, schedule::TimelineItemDisplay::Bar);
}

void TstTimelineOracle::propsEditRoundTrips()
{
    const QString path = fixture(QStringLiteral("tl_01_one_bar_task.mpp"));
    if (path.isEmpty())
        QSKIP("tl_01 fixture not installed");
    MppIO io;
    QVERIFY2(io.open(path), qPrintable(io.errorString()));
    schedule::Project p = io.project();
    QVERIFY(p.timelineView.showTodayLine);         // default in the fixture
    QVERIFY(p.timelineView.showPanZoom);

    p.timelineView.showTodayLine = false;
    p.timelineView.numTextLines = 3;
    p.timelineView.touch();

    const schedule::Project rp = reload(p);
    QVERIFY(rp.timelineView.present);
    QCOMPARE(rp.timelineView.showTodayLine, false);
    QCOMPARE(rp.timelineView.numTextLines, 3);
    QCOMPARE(rp.timelineView.showPanZoom, true);    // untouched
}

void TstTimelineOracle::barAssignmentRoundTrips()
{
    const QString path = fixture(QStringLiteral("tl_05_two_bars.mpp"));
    if (path.isEmpty())
        QSKIP("tl_05 fixture not installed");
    MppIO io;
    QVERIFY2(io.open(path), qPrintable(io.errorString()));
    schedule::Project p = io.project();

    int moved = -1;
    for (schedule::TimelineItem &it : p.timelineView.items) {
        if (it.barId == 1) {
            it.barId = 2;
            moved = it.taskUid;
            break;
        }
    }
    QVERIFY(moved > 0);
    p.timelineView.touch();

    const schedule::Project rp = reload(p);
    bool seen = false;
    for (const schedule::TimelineItem &it : rp.timelineView.items)
        if (it.taskUid == moved) {
            QCOMPARE(it.barId, 2);
            seen = true;
        }
    QVERIFY2(seen, "moved member missing after reload");
}

void TstTimelineOracle::untouchedWhenOtherViewPatched()
{
    const QString path = fixture(QStringLiteral("tl_01_one_bar_task.mpp"));
    if (path.isEmpty())
        QSKIP("tl_01 fixture not installed");
    MppIO io;
    QVERIFY2(io.open(path), qPrintable(io.errorString()));
    schedule::Project p = io.project();
    const QByteArray originalXml = p.timelineView.rawXml;
    QVERIFY(!originalXml.isEmpty());

    // Patch an unrelated view; leave timelineView.modified false.
    p.viewStyles.present = true;
    p.viewStyles.text[schedule::ViewStyles::Critical].bold = true;

    const schedule::Project rp = reload(p);
    QVERIFY2(rp.timelineView.present, "timeline lost when another view was patched");
    QCOMPARE(rp.timelineView.rawXml, originalXml);
}

// The hand-built "Timeline Test.mpp" carries a fully formatted timeline (8 real
// members, custom per-item <fmt>, a populated <txtSet>). No manifest -- just
// assert the parse produced a coherent model.
void TstTimelineOracle::richFixtureDecodes()
{
    QString path;
    for (const QString &mpp : fixtures::mppFiles())
        if (QFileInfo(mpp).fileName() == QStringLiteral("Timeline Test.mpp"))
            path = mpp;
    if (path.isEmpty())
        QSKIP("Timeline Test.mpp not installed");

    MppIO io;
    QVERIFY2(io.open(path), qPrintable(io.errorString()));
    const schedule::TimelineViewSettings &tv = io.project().timelineView;

    QVERIFY(tv.present);
    QCOMPARE(tv.items.size(), 8);
    for (const schedule::TimelineItem &it : tv.items) {
        QVERIFY(it.taskUid > 0);
        QVERIFY(it.onTimeline);
        QVERIFY(!it.guid.isEmpty());
        QVERIFY(tv.bar(it.barId) != nullptr);
    }
    QVERIFY2(tv.textStyles.size() >= 12, "expected the full <txtSet> category set");
    QVERIFY(tv.bar(0) != nullptr);
    // Round-trips through rawXml unchanged.
    QVERIFY(!tv.rawXml.isEmpty());
}

void TstTimelineOracle::timelineMatchesManifest_data()
{
    QTest::addColumn<QString>("mpp");
    QTest::addColumn<QString>("manifest");
    for (const QString &mpp : fixtures::mppFiles()) {
        const QFileInfo fi(mpp);
        if (!fi.completeBaseName().startsWith(QStringLiteral("tl_")))
            continue;
        const QString manifest = fi.dir().filePath(QStringLiteral("manifest.json"));
        if (QFile::exists(manifest))
            QTest::newRow(qPrintable(fixtures::label(mpp))) << mpp << manifest;
    }
}

void TstTimelineOracle::timelineMatchesManifest()
{
    QFETCH(QString, mpp);
    QFETCH(QString, manifest);

    QFile mf(manifest);
    QVERIFY(mf.open(QIODevice::ReadOnly));
    const QJsonObject man = QJsonDocument::fromJson(mf.readAll()).object();

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));
    const schedule::TimelineViewSettings &tv = io.project().timelineView;

    QVERIFY2(tv.present, "timelineView not decoded");
    QVERIFY(!tv.rawXml.isEmpty());
    QVERIFY2(tv.viewUid >= 0, "no timeline view uid");
    // Every file has the internal default bar plus the one visible bar.
    QVERIFY2(tv.bar(0) != nullptr, "missing internal <tl id=0>");
    QVERIFY2(tv.bar(1) != nullptr, "missing visible <tl id=1>");

    // --- membership ---------------------------------------------------------
    QSet<int> got;
    for (const schedule::TimelineItem &it : tv.items) {
        QVERIFY2(it.onTimeline, "member with onTL=0 was kept");
        got.insert(it.taskUid);
    }
    QSet<int> want;
    for (const QJsonValue &v : man.value(QStringLiteral("onTimeline")).toArray())
        want.insert(v.toInt());
    QCOMPARE(got, want);

    // --- bars -------------------------------------------------------------
    if (man.contains(QStringLiteral("bars"))) {
        int visible = 0;
        for (const schedule::TimelineBar &b : tv.bars)
            if (b.id >= 1)
                ++visible;
        QCOMPARE(visible, man.value(QStringLiteral("bars")).toInt());
    }
    const QJsonObject labels = man.value(QStringLiteral("barLabels")).toObject();
    for (auto it = labels.begin(); it != labels.end(); ++it) {
        const schedule::TimelineBar *b = tv.bar(it.key().toInt() + 1);
        QVERIFY2(b, qPrintable(QStringLiteral("no bar for index %1").arg(it.key())));
        QCOMPARE(b->label, it.value().toString());
    }
    if (man.contains(QStringLiteral("customRange"))) {
        const QJsonObject cr = man.value(QStringLiteral("customRange")).toObject();
        const schedule::TimelineBar *b = tv.bar(cr.value(QStringLiteral("bar")).toInt() + 1);
        QVERIFY(b);
        QVERIFY2(b->useCustomDates, "custom range not flagged");
        QCOMPARE(b->customStart.toString(Qt::ISODate), cr.value(QStringLiteral("start")).toString());
        QCOMPARE(b->customFinish.toString(Qt::ISODate), cr.value(QStringLiteral("finish")).toString());
    }
    // Members land on the bar the generator asked for.
    const QJsonObject barOf = man.value(QStringLiteral("barOf")).toObject();
    for (auto it = barOf.begin(); it != barOf.end(); ++it) {
        bool found = false;
        for (const schedule::TimelineItem &m : tv.items) {
            if (m.taskUid == it.key().toInt()) {
                QCOMPARE(m.barId, it.value().toInt() + 1);
                found = true;
            }
        }
        QVERIFY2(found, qPrintable(QStringLiteral("uid %1 not on the timeline").arg(it.key())));
    }

    // --- options --------------------------------------------------------
    const QJsonObject opt = man.value(QStringLiteral("option")).toObject();
    for (auto it = opt.begin(); it != opt.end(); ++it) {
        const QString k = it.key();
        if (k == QStringLiteral("showToday"))
            QCOMPARE(tv.showTodayLine, it.value().toBool());
        else if (k == QStringLiteral("showTimescale"))
            QCOMPARE(tv.showTimescale, it.value().toBool());
        else if (k == QStringLiteral("showPanZoom"))
            QCOMPARE(tv.showPanZoom, it.value().toBool());
        else if (k == QStringLiteral("showOverlaps"))
            QCOMPARE(tv.showOverlaps, it.value().toBool());
        else if (k == QStringLiteral("numLines"))
            QCOMPARE(tv.numTextLines, it.value().toInt());
    }

    // --- milestone flag join --------------------------------------------
    for (const QJsonValue &v : man.value(QStringLiteral("milestones")).toArray()) {
        bool found = false;
        for (const schedule::TimelineItem &m : tv.items)
            if (m.taskUid == v.toInt()) {
                QVERIFY2(m.milestone, "timeline milestone not flagged");
                found = true;
            }
        QVERIFY(found);
    }

    // --- display style (bar / callout) --------------------------------
    const QJsonObject disp = man.value(QStringLiteral("display")).toObject();
    for (auto it = disp.begin(); it != disp.end(); ++it) {
        bool found = false;
        for (const schedule::TimelineItem &m : tv.items)
            if (m.taskUid == it.key().toInt()) {
                const bool wantCallout = it.value().toString() == QLatin1String("callout");
                QCOMPARE(m.display == schedule::TimelineItemDisplay::Callout, wantCallout);
                found = true;
            }
        QVERIFY2(found, qPrintable(QStringLiteral("display: uid %1 not on the timeline").arg(it.key())));
    }
}

void TstTimelineOracle::noOpEditIsStable_data()
{
    QTest::addColumn<QString>("file");
    for (const char *f : { "tl_02_many_bar_tasks.mpp", "tl_06_bar_label_range.mpp",
                           "tl_12_text_styles.mpp", "Timeline Test.mpp" })
        QTest::newRow(f) << QString::fromLatin1(f);
}

// Marking the timeline modified but changing nothing must not perturb it: the
// writer's "serialized == rawXml" guard should route it through the verbatim
// copy, leaving the <TLViewData> bytes and the model identical.
void TstTimelineOracle::noOpEditIsStable()
{
    QFETCH(QString, file);
    const QString path = fixture(file);
    if (path.isEmpty())
        QSKIP("fixture not installed");
    MppIO io;
    QVERIFY2(io.open(path), qPrintable(io.errorString()));
    schedule::Project p = io.project();
    QVERIFY(p.timelineView.present);
    const QByteArray originalXml = p.timelineView.rawXml;
    const auto originalItems = p.timelineView.items;
    const auto originalBars = p.timelineView.bars;

    p.timelineView.touch();   // modified = true, but no field changed

    const schedule::Project rp = reload(p);
    QVERIFY(rp.timelineView.present);
    QCOMPARE(rp.timelineView.rawXml, originalXml);
    QCOMPARE(rp.timelineView.items.size(), originalItems.size());
    QCOMPARE(rp.timelineView.bars.size(), originalBars.size());
}

QTEST_MAIN(TstTimelineOracle)
#include "tst_timeline_oracle.moc"
