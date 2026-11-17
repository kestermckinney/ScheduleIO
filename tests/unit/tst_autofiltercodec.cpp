// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Column AutoFilters against files Microsoft Project 2016 wrote with
// SetAutoFilter (tests/fixtures/mpp_samples/autofilter, made by ScheduleVault's
// tools/autofilter-samples-remote.vbs): each decodes to the filter the
// sample applied, re-encoding gives back Project's layout byte for byte (bar
// the uninitialised bytes Project leaves in its criteria blocks), and a filter
// written into a file reads back with Project's AutoFilter switch on.

#include "codec/autofiltercodec.h"
#include "codec/bkndvardata.h"
#include "mppio.h"
#include "ole/compoundfile.h"

#include <QFile>
#include <QTest>
#include <QtEndian>

using schedule::AutoFilterCondition;
using schedule::AutoFilterValue;
using schedule::ColumnAutoFilter;

namespace {

const QString kDir = QStringLiteral(SCHEDULEIO_FIXTURE_DIR "/mpp_samples/autofilter/");
constexpr quint32 kName = 0x0B40000E, kStart = 0x0B400505, kFinish = 0x0B400506,
                  kDuration = 0x0B400507, kWork = 0x0B400000, kPercent = 0x0B400020,
                  kCost = 0x0B400005, kFlag1 = 0x0B400048, kResourceName = 0x0C400001;
constexpr qint64 kDay = 8LL * 3600 * 1000;

quint32 u32(const QByteArray &b, int o)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + o));
}

QByteArray fixture(const QString &name)
{
    QFile f(kDir + name);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = f.readAll();
    return bytes.startsWith("version https://git-lfs") ? QByteArray() : bytes;
}

schedule::Project open(const QString &name)
{
    MppIO io;
    if (!io.openFromData(fixture(name)))
        return {};
    return io.project();
}

// The raw AutoFilter item of every view in the file, by view uid.
QHash<quint32, QByteArray> rawItems(const QByteArray &file)
{
    QHash<quint32, QByteArray> out;
    CompoundFile cf;
    if (!cf.openFromData(file))
        return out;
    const QString s = QStringLiteral("   214"), v = QStringLiteral("CV_iew");
    BkndVarData vd;
    if (!vd.parse(cf.readStream({ s, v, QStringLiteral("VarMeta") }),
                  cf.readStream({ s, v, QStringLiteral("Var2Data") })))
        return out;
    for (quint32 uid = 1; uid < 16; ++uid) {
        const QByteArray blob = vd.blobFor(uid, 6);
        for (int o = 16; o + 12 <= blob.size(); ) {
            const quint32 len = u32(blob, o), key = u32(blob, o + 4);
            if (o + 12 + int(len) > blob.size())
                break;
            if (key == AutoFilterCodec::kPropsKey)
                out.insert(uid, blob.mid(o + 12, int(len)));
            o += 12 + int(len) + int(len & 1);
        }
    }
    return out;
}

QByteArray ganttItem(const QString &name) { return rawItems(fixture(name)).value(2); }

AutoFilterValue text(const QString &t) { AutoFilterValue v; v.text = t; return v; }

} // namespace

class TstAutoFilterCodec : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void unfilteredViewHasNoFilters();
    void decodesText();
    void decodesOrAndValueList();
    void decodesNumbersDatesAndDurations();
    void decodesTwoColumnsAndOtherViews();
    void reencodesProjectsLayout_data();
    void reencodesProjectsLayout();
    void writtenFiltersReadBackWithTheSwitchOn();
    void clearingRemovesTheFilters();
};

void TstAutoFilterCodec::initTestCase()
{
    if (fixture(QStringLiteral("af-none.mpp")).isEmpty())
        QSKIP("AutoFilter fixtures missing (run git lfs pull)");
}

void TstAutoFilterCodec::unfilteredViewHasNoFilters()
{
    const schedule::Project p = open(QStringLiteral("af-none.mpp"));
    QVERIFY(p.ganttView.present);
    QVERIFY(p.ganttView.autoFilters.isEmpty());
}

void TstAutoFilterCodec::decodesText()
{
    const auto f = open(QStringLiteral("af-name-contains.mpp")).ganttView.autoFilters;
    QCOMPARE(f.size(), 1);
    QCOMPARE(f[0].fieldId, kName);
    QCOMPARE(f[0].filterType, int(ColumnAutoFilter::Custom));
    QCOMPARE(f[0].conditions.size(), 1);
    QCOMPARE(f[0].conditions[0].test, quint16(AutoFilterCondition::Contains));
    QCOMPARE(f[0].conditions[0].values, QList<AutoFilterValue>{ text(QStringLiteral("De")) });

    const auto ne = open(QStringLiteral("af-name-notequals.mpp")).ganttView.autoFilters;
    QCOMPARE(ne[0].conditions[0].test, quint16(AutoFilterCondition::NotEquals));
    QCOMPARE(ne[0].conditions[0].values[0].text, QStringLiteral("Build"));
}

void TstAutoFilterCodec::decodesOrAndValueList()
{
    const auto f = open(QStringLiteral("af-name-or.mpp")).ganttView.autoFilters;
    QCOMPARE(f.size(), 1);
    QVERIFY(f[0].orJoin);
    QCOMPARE(f[0].conditions.size(), 2);
    QCOMPARE(f[0].conditions[0].values[0].text, QStringLiteral("Build"));
    QCOMPARE(f[0].conditions[1].values[0].text, QStringLiteral("Test"));
    QCOMPARE(f[0].conditions[1].test, quint16(AutoFilterCondition::Equals));

    const auto in = open(QStringLiteral("af-name-in.mpp")).ganttView.autoFilters;
    QCOMPARE(in.size(), 1);
    QCOMPARE(in[0].filterType, int(ColumnAutoFilter::In));
    QCOMPARE(in[0].list, (QStringList{ QStringLiteral("Build"), QStringLiteral("Test") }));
}

void TstAutoFilterCodec::decodesNumbersDatesAndDurations()
{
    auto one = [](const char *file) {
        const auto f = open(QString::fromLatin1(file)).ganttView.autoFilters;
        return f.isEmpty() ? ColumnAutoFilter() : f.first();
    };

    ColumnAutoFilter d = one("af-dur-greater.mpp");
    QCOMPARE(d.fieldId, kDuration);
    QCOMPARE(d.conditions[0].test, quint16(AutoFilterCondition::Greater));
    QCOMPARE(d.conditions[0].values[0].type, AutoFilterValue::Type::Duration);
    QCOMPARE(d.conditions[0].values[0].millis, 3 * kDay);
    QCOMPARE(d.conditions[0].values[0].durationUnit, 7);

    d = one("af-dur-1week.mpp");
    QCOMPARE(d.filterType, 24);
    QCOMPARE(d.conditions[0].test, quint16(AutoFilterCondition::GreaterEq));
    QCOMPARE(d.conditions[0].values[0].millis, 5 * kDay);

    d = one("af-work-8h.mpp");
    QCOMPARE(d.fieldId, kWork);
    QCOMPARE(d.filterType, 26);
    QCOMPARE(d.conditions[0].test, quint16(AutoFilterCondition::LessEq));
    QCOMPARE(d.conditions[0].values[0].type, AutoFilterValue::Type::Work);
    QCOMPARE(d.conditions[0].values[0].millis, kDay);

    d = one("af-start-within.mpp");
    QCOMPARE(d.fieldId, kStart);
    QVERIFY(!d.orJoin);
    QCOMPARE(d.conditions.size(), 2);
    QCOMPARE(d.conditions[0].test, quint16(AutoFilterCondition::GreaterEq));
    QCOMPARE(d.conditions[1].test, quint16(AutoFilterCondition::LessEq));
    QCOMPARE(d.conditions[0].values[0].type, AutoFilterValue::Type::Date);
    QCOMPARE(d.conditions[0].values[0].date.time(), QTime(8, 0));
    QCOMPARE(d.conditions[0].values[0].date.daysTo(d.conditions[1].values[0].date), 14);

    d = one("af-finish-greater.mpp");
    QCOMPARE(d.fieldId, kFinish);

    d = one("af-pct-complete.mpp");
    QCOMPARE(d.fieldId, kPercent);
    QCOMPARE(d.filterType, 32);
    QCOMPARE(d.conditions[0].values[0].type, AutoFilterValue::Type::Percent);
    QCOMPARE(d.conditions[0].values[0].number, 100.0);

    d = one("af-pct-inprogress.mpp");
    QCOMPARE(d.conditions.size(), 2);
    QCOMPARE(d.conditions[0].test, quint16(AutoFilterCondition::Greater));
    QCOMPARE(d.conditions[1].test, quint16(AutoFilterCondition::Less));

    d = one("af-cost-gt0.mpp");
    QCOMPARE(d.fieldId, kCost);
    QCOMPARE(d.filterType, 37);
    QCOMPARE(d.conditions[0].values[0].type, AutoFilterValue::Type::Number);
    QCOMPARE(d.conditions[0].values[0].number, 0.0);

    d = one("af-flag1-yes.mpp");
    QCOMPARE(d.fieldId, kFlag1);
    QCOMPARE(d.filterType, 38);
    QCOMPARE(d.conditions[0].values[0].type, AutoFilterValue::Type::Flag);
    QCOMPARE(d.conditions[0].values[0].number, 1.0);
}

void TstAutoFilterCodec::decodesTwoColumnsAndOtherViews()
{
    const auto two = open(QStringLiteral("af-two-columns.mpp")).ganttView.autoFilters;
    QCOMPARE(two.size(), 2);
    QCOMPARE(two[0].fieldId, kName);
    QCOMPARE(two[1].fieldId, kDuration);

    const schedule::Project usage = open(QStringLiteral("af-taskusage-name.mpp"));
    QVERIFY(usage.ganttView.autoFilters.isEmpty());
    QCOMPARE(usage.taskUsageView.autoFilters.size(), 1);
    QCOMPARE(usage.taskUsageView.autoFilters[0].conditions[0].values[0].text, QStringLiteral("De"));

    const schedule::Project sheet = open(QStringLiteral("af-ressheet-name.mpp"));
    QCOMPARE(sheet.resourceSheetView.autoFilters.size(), 1);
    QCOMPARE(sheet.resourceSheetView.autoFilters[0].fieldId, kResourceName);
}

void TstAutoFilterCodec::reencodesProjectsLayout_data()
{
    QTest::addColumn<QString>("file");
    for (const char *f : { "af-none.mpp", "af-name-contains.mpp", "af-name-equals.mpp",
                           "af-name-or.mpp", "af-name-in.mpp", "af-start-within.mpp",
                           "af-start-thisweek.mpp", "af-dur-1day.mpp", "af-dur-none.mpp",
                           "af-work-8h.mpp", "af-pct-inprogress.mpp", "af-cost-gt0.mpp",
                           "af-flag1-yes.mpp", "af-two-columns.mpp" })
        QTest::newRow(f) << QString::fromLatin1(f);
}

void TstAutoFilterCodec::reencodesProjectsLayout()
{
    QFETCH(QString, file);
    const QByteArray item = ganttItem(file);
    QVERIFY(!item.isEmpty());
    const auto filters = AutoFilterCodec::decode(item);
    const QByteArray again = AutoFilterCodec::encode(
        filters, AutoFilterCodec::fromMppDate(u32(item, 4)));
    QCOMPARE(AutoFilterCodec::decode(again), filters);
    if (file == QLatin1String("af-dur-none.mpp"))
        return;   // Project's "No Duration" second test compares to an untyped blank
    QCOMPARE(again.size(), item.size());

    // Every byte but the ones Project leaves uninitialised: field and value
    // blocks' u32@8, and the bytes after a value's payload.
    auto masked = [](QByteArray b) {
        int o = 16;
        const int count = int(u32(b, 8));
        for (int e = 0; e < count && o + 32 <= b.size(); ++e) {
            const int base = o + 12;
            const int blocks = qFromLittleEndian<quint16>(
                reinterpret_cast<const uchar *>(b.constData() + base + 14));
            for (int i = 0; i < blocks; ++i) {
                const int at = base + 0x14 + i * 36;
                const quint32 kind = u32(b, at);
                if (kind != 7 && kind != 1 && i > 1) {
                    for (int k = 8; k < 26; ++k) b[at + k] = 0;   // a second test
                } else if (kind == 7) {
                    for (int k = 12; k < 26; ++k) b[at + k] = 0;
                } else if (kind == 1) {
                    for (int k = 8; k < 12; ++k) b[at + k] = 0;
                    const quint16 type = qFromLittleEndian<quint16>(
                        reinterpret_cast<const uchar *>(b.constData() + at + 12));
                    const int payload = type == 5 ? 8 : type == 0x13 ? 4 : type == 8 || type == 0x4000 ? 0 : 2;
                    for (int k = 14 + payload; k < 24; ++k) b[at + k] = 0;
                }
            }
            o += int(u32(b, o + 4));
            // An In entry's value list follows it (and only the last entry
            // is checked here, so it need not be skipped).
        }
        return b;
    };
    QCOMPARE(masked(again).toHex(' '), masked(item).toHex(' '));
}

void TstAutoFilterCodec::writtenFiltersReadBackWithTheSwitchOn()
{
    MppIO io;
    QVERIFY(io.openFromData(fixture(QStringLiteral("af-off.mpp"))));
    schedule::Project p = io.project();

    ColumnAutoFilter name;
    name.fieldId = kName;
    AutoFilterCondition c;
    c.test = AutoFilterCondition::Equals;
    c.values = { text(QStringLiteral("T*")) };
    name.conditions = { c };

    ColumnAutoFilter dur;
    dur.fieldId = kDuration;
    dur.filterType = 24;
    AutoFilterValue week;
    week.type = AutoFilterValue::Type::Duration;
    week.millis = 5 * kDay;
    AutoFilterCondition ge;
    ge.test = AutoFilterCondition::GreaterEq;
    ge.values = { week };
    dur.conditions = { ge };

    ColumnAutoFilter list;
    list.fieldId = kName;
    list.filterType = ColumnAutoFilter::In;
    list.list = { QStringLiteral("Build"), QStringLiteral("Test") };

    p.ganttView.setAutoFilters({ name, dur });
    p.resourceUsageView.setAutoFilters({});
    io.setProject(p);
    const QByteArray saved = io.saveToData();
    QVERIFY2(!saved.isEmpty(), qPrintable(io.errorString()));

    MppIO back;
    QVERIFY(back.openFromData(saved));
    QCOMPARE(back.project().ganttView.autoFilters, (QList<ColumnAutoFilter>{ name, dur }));

    CompoundFile cf;
    QVERIFY(cf.openFromData(saved));
    const QByteArray props = cf.readStream({ QStringLiteral("   214"), QStringLiteral("Props") });
    bool on = false;
    for (int o = 16; o + 12 <= props.size(); ) {
        const quint32 len = u32(props, o), key = u32(props, o + 4);
        if (key == AutoFilterCodec::kSwitchKey)
            on = props.at(o + 12) == 1;
        o += 12 + int(len) + int(len & 1);
    }
    QVERIFY2(on, "the AutoFilter switch is on");

    // A value list survives a save too.
    p.ganttView.setAutoFilters({ list });
    io.setProject(p);
    MppIO listed;
    QVERIFY(listed.openFromData(io.saveToData()));
    QCOMPARE(listed.project().ganttView.autoFilters, QList<ColumnAutoFilter>{ list });
}

void TstAutoFilterCodec::clearingRemovesTheFilters()
{
    MppIO io;
    QVERIFY(io.openFromData(fixture(QStringLiteral("af-two-columns.mpp"))));
    schedule::Project p = io.project();
    QCOMPARE(p.ganttView.autoFilters.size(), 2);
    p.ganttView.setAutoFilters({});
    io.setProject(p);
    MppIO back;
    QVERIFY(back.openFromData(io.saveToData()));
    QVERIFY(back.project().ganttView.autoFilters.isEmpty());
}

QTEST_GUILESS_MAIN(TstAutoFilterCodec)
#include "tst_autofiltercodec.moc"
