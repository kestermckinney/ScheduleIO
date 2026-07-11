// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// materializeResourceCalendars / collapseResourceCalendarPassThroughs: the
// write-time synthesis of per-resource derived calendar rows (the only way
// the .mpp format records a resource's calendar) and the read-side collapse
// back to plain resource -> base-calendar references.

#include "model/calendar.h"
#include "model/project.h"
#include "mppio.h"

#include <QTemporaryDir>
#include <QTest>

using schedule::Calendar;
using schedule::Project;
using schedule::Resource;
using schedule::TimeRange;

namespace {

Project projectWithDefaults()
{
    Project p;
    p.formatVersion = Project::FormatVersion::Mpp14;
    p.startDate = QDateTime(QDate(2026, 7, 6), QTime(8, 0));
    p.calendars = Calendar::microsoftDefaults();
    p.calendarUniqueId = 1;
    return p;
}

Resource resource(int uid, const QString &name, int calUid)
{
    Resource r;
    r.uniqueId = uid;
    r.id = uid;
    r.name = name;
    r.calendarUniqueId = calUid;
    return r;
}

const Calendar *calByUid(const Project &p, int uid)
{
    for (const Calendar &c : p.calendars)
        if (c.uniqueId == uid)
            return &c;
    return nullptr;
}

// The shape a synthesized row has after a read from a real file: no data
// blob means the reader defaults the mask to 0x1F with all-empty day lists.
void applyReadBackShape(Project &p)
{
    for (Calendar &c : p.calendars)
        if (c.baseCalendarUniqueId >= 0 && c.exceptions.isEmpty()) {
            bool empty = true;
            for (const QList<TimeRange> &day : c.workingTimes)
                if (!day.isEmpty()) { empty = false; break; }
            if (empty)
                c.workingDayMask = 0x1F;
        }
}

} // namespace

class TstResourceCalendars : public QObject
{
    Q_OBJECT
private slots:
    void materializeCreatesDerivedRow();
    void materializeIsIdempotent();
    void materializeSkips();
    void materializeDuplicateNames();
    void collapseRestoresReference();
    void collapseGuards();
    void materializeThenCollapseInverts();
    void binaryRoundTripCollapses();
};

void TstResourceCalendars::materializeCreatesDerivedRow()
{
    Project p = projectWithDefaults();
    p.resources.append(resource(1, QStringLiteral("Alice"), 1));

    schedule::materializeResourceCalendars(p);

    QCOMPARE(p.calendars.size(), 4);
    const Calendar &row = p.calendars.last();
    QCOMPARE(row.uniqueId, 4);
    QCOMPARE(row.name, QStringLiteral("Alice"));
    QCOMPARE(row.baseCalendarUniqueId, 1);
    QVERIFY(row.workingTimes.isEmpty());
    QVERIFY(row.exceptions.isEmpty());
    QCOMPARE(p.resources[0].calendarUniqueId, 4);
}

void TstResourceCalendars::materializeIsIdempotent()
{
    Project p = projectWithDefaults();
    p.resources.append(resource(1, QStringLiteral("Alice"), 1));
    schedule::materializeResourceCalendars(p);
    const Project once = p;
    schedule::materializeResourceCalendars(p);
    QCOMPARE(p.calendars, once.calendars);
    QCOMPARE(p.resources, once.resources);
}

void TstResourceCalendars::materializeSkips()
{
    Project p = projectWithDefaults();
    p.resources.append(resource(1, QStringLiteral("None"), -1));    // "(None)"
    p.resources.append(resource(2, QStringLiteral("Ghost"), 99));   // unknown uid
    Calendar derived;
    derived.uniqueId = 7;
    derived.name = QStringLiteral("Bob");
    derived.baseCalendarUniqueId = 1;
    p.calendars.append(derived);
    p.resources.append(resource(3, QStringLiteral("Bob"), 7));      // already derived

    const Project before = p;
    schedule::materializeResourceCalendars(p);
    QCOMPARE(p.calendars, before.calendars);
    QCOMPARE(p.resources, before.resources);

    // And an empty calendar list is left alone entirely.
    Project bare;
    bare.resources.append(resource(1, QStringLiteral("Solo"), 1));
    schedule::materializeResourceCalendars(bare);
    QVERIFY(bare.calendars.isEmpty());
    QCOMPARE(bare.resources[0].calendarUniqueId, 1);
}

void TstResourceCalendars::materializeDuplicateNames()
{
    Project p = projectWithDefaults();
    p.resources.append(resource(1, QStringLiteral("Bob"), 1));
    p.resources.append(resource(2, QStringLiteral("Bob"), 1));

    schedule::materializeResourceCalendars(p);

    QCOMPARE(p.calendars.size(), 5);
    QCOMPARE(p.resources[0].calendarUniqueId, 4);
    QCOMPARE(p.resources[1].calendarUniqueId, 5);
    QCOMPARE(calByUid(p, 4)->name, QStringLiteral("Bob"));
    QCOMPARE(calByUid(p, 5)->name, QStringLiteral("Bob"));
}

void TstResourceCalendars::collapseRestoresReference()
{
    Project p = projectWithDefaults();
    p.resources.append(resource(1, QStringLiteral("Alice"), 1));
    schedule::materializeResourceCalendars(p);
    applyReadBackShape(p);   // mask 0x1F must not block the collapse

    schedule::collapseResourceCalendarPassThroughs(p);

    QCOMPARE(p.calendars.size(), 3);
    QCOMPARE(p.resources[0].calendarUniqueId, 1);
}

void TstResourceCalendars::collapseGuards()
{
    Project p = projectWithDefaults();

    // Customized derived row (explicit hours): kept.
    Calendar custom;
    custom.uniqueId = 10;
    custom.name = QStringLiteral("Custom");
    custom.baseCalendarUniqueId = 1;
    custom.workingTimes.resize(7);
    custom.workingTimes[0] = { TimeRange{ QTime(6, 0), QTime(14, 0) } };
    p.calendars.append(custom);
    p.resources.append(resource(1, QStringLiteral("Custom"), 10));

    // Pass-through row referenced by TWO resources: kept.
    Calendar shared;
    shared.uniqueId = 11;
    shared.name = QStringLiteral("Shared");
    shared.baseCalendarUniqueId = 1;
    p.calendars.append(shared);
    p.resources.append(resource(2, QStringLiteral("R2"), 11));
    p.resources.append(resource(3, QStringLiteral("R3"), 11));

    // Pass-through row that is a TASK's calendar: kept.
    Calendar taskCal;
    taskCal.uniqueId = 12;
    taskCal.baseCalendarUniqueId = 1;
    p.calendars.append(taskCal);
    p.resources.append(resource(4, QStringLiteral("R4"), 12));
    schedule::Task t;
    t.uniqueId = 1;
    t.calendarUniqueId = 12;
    p.tasks.append(t);

    // Pass-through row that is the BASE of another calendar: kept.
    Calendar parent;
    parent.uniqueId = 13;
    parent.baseCalendarUniqueId = 1;
    p.calendars.append(parent);
    Calendar child;
    child.uniqueId = 14;
    child.baseCalendarUniqueId = 13;
    p.calendars.append(child);
    p.resources.append(resource(5, QStringLiteral("R5"), 13));

    // Pass-through row whose base is itself derived: kept.
    p.resources.append(resource(6, QStringLiteral("R6"), 14));

    // Unreferenced empty derived orphan: kept (refcount 0).
    Calendar orphan;
    orphan.uniqueId = 15;
    orphan.baseCalendarUniqueId = 1;
    p.calendars.append(orphan);

    // The project-default calendar: kept even when derived + referenced once.
    Calendar def;
    def.uniqueId = 16;
    def.baseCalendarUniqueId = 1;
    p.calendars.append(def);
    p.resources.append(resource(7, QStringLiteral("R7"), 16));
    p.calendarUniqueId = 16;

    const Project before = p;
    schedule::collapseResourceCalendarPassThroughs(p);
    QCOMPARE(p.calendars, before.calendars);
    QCOMPARE(p.resources, before.resources);
}

void TstResourceCalendars::materializeThenCollapseInverts()
{
    Project p = projectWithDefaults();
    p.resources.append(resource(1, QStringLiteral("Alice"), 1));
    p.resources.append(resource(2, QStringLiteral("Bob"), 3));   // Night Shift
    const Project original = p;

    schedule::materializeResourceCalendars(p);
    applyReadBackShape(p);
    schedule::collapseResourceCalendarPassThroughs(p);

    QCOMPARE(p.calendars, original.calendars);
    QCOMPARE(p.resources, original.resources);
}

void TstResourceCalendars::binaryRoundTripCollapses()
{
    Project p = projectWithDefaults();
    schedule::Task t;
    t.uniqueId = 1;
    t.id = 1;
    t.name = QStringLiteral("Task");
    t.start = QDateTime(QDate(2026, 7, 6), QTime(8, 0));
    t.finish = QDateTime(QDate(2026, 7, 7), QTime(17, 0));
    t.durationMillis = 16LL * 3600 * 1000;
    p.tasks.append(t);
    p.resources.append(resource(1, QStringLiteral("Alice"), 1));
    p.resources.append(resource(2, QStringLiteral("Bob"), 1));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path1 = dir.filePath(QStringLiteral("one.mpp"));

    MppIO io;
    io.setProject(p);
    QVERIFY(io.save(path1));

    MppIO in1;
    QVERIFY(in1.open(path1));
    Project readBack = in1.project();
    // The file carries one derived row per resource, resID-linked.
    QCOMPARE(readBack.calendars.size(), p.calendars.size() + 2);
    const Calendar *aliceCal = calByUid(readBack, readBack.resources[0].calendarUniqueId);
    QVERIFY(aliceCal);
    QCOMPARE(aliceCal->name, QStringLiteral("Alice"));
    QCOMPARE(aliceCal->baseCalendarUniqueId, 1);

    // Collapse restores plain references to Standard.
    schedule::collapseResourceCalendarPassThroughs(readBack);
    QCOMPARE(readBack.calendars.size(), p.calendars.size());
    QCOMPARE(readBack.resources[0].calendarUniqueId, 1);
    QCOMPARE(readBack.resources[1].calendarUniqueId, 1);

    // Saving the collapsed model reproduces the same calendar structure.
    const QString path2 = dir.filePath(QStringLiteral("two.mpp"));
    MppIO io2;
    io2.setProject(readBack);
    QVERIFY(io2.save(path2));
    MppIO in2;
    QVERIFY(in2.open(path2));
    QCOMPARE(in2.project().calendars, in1.project().calendars);
    QCOMPARE(in2.project().resources, in1.project().resources);
}

QTEST_MAIN(TstResourceCalendars)
#include "tst_resourcecalendars.moc"
