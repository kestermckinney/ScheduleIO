// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"
#include "xmlio.h"

#include "codec/fielddecoders.h"
#include "ole/compoundfile.h"

#include <QSet>
#include <QTest>

// A predecessor link with a missing/zero PredecessorUID, or one that names its
// own task, is never valid: task uid 0 is always the Project Summary Task, and
// a self-link is a trivial cycle. If either reaches the MPP14 writer's
// TBkndCons table, Microsoft Project rejects the saved file with a circular
// reference error naming that task -- "Empty" in projects built on
// ScheduleIO's own template, since its uid-0 summary task is named that.
//
// These tests prove: (1) the MSPDI/.xml reader drops such links instead of
// letting them into the model, and (2) the MPP14 writer refuses to emit a bad
// TBkndCons row even if a bad Relation reaches it by some other path, so the
// two failure entry points are both closed.
class TstXmlPredecessorGuard : public QObject
{
    Q_OBJECT
private slots:
    void readerDropsInvalidLinks();
    void writerNeverEmitsBadRow();
    void endToEndBrokenXmlToMpp();
};

namespace {

// Task uid 0 is named "Empty" here on purpose, mirroring ScheduleIO's own
// tests/fixtures/Empty.xml template -- the exact task MS Project names in the
// user-reported error.
const char *kBrokenLinksXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<Project>"
      "<Tasks>"
        "<Task><UID>0</UID><ID>0</ID><Name>Empty</Name><Summary>1</Summary></Task>"
        "<Task><UID>1</UID><ID>1</ID><Name>Task A</Name></Task>"
        "<Task>"
          "<UID>2</UID><ID>2</ID><Name>Task B</Name>"
          "<PredecessorLink><Type>1</Type></PredecessorLink>"
          "<PredecessorLink><PredecessorUID>0</PredecessorUID><Type>1</Type></PredecessorLink>"
          "<PredecessorLink><PredecessorUID>2</PredecessorUID><Type>1</Type></PredecessorLink>"
          "<PredecessorLink><PredecessorUID>1</PredecessorUID><Type>1</Type></PredecessorLink>"
        "</Task>"
      "</Tasks>"
    "</Project>";

struct ConsRow { quint32 uid, pred, succ; };

// Decodes the raw TBkndCons FixedMeta/FixedData streams the same way
// docserializer.cpp's readRealRelations does (20-byte records; u16 dead flag
// + u32 offset per 10-byte meta entry) -- but WITHOUT that reader's own
// zero/self-link guard, so a writer regression that still emitted a bad row
// isn't masked by the reader silently dropping it again on the way back in.
QList<ConsRow> rawConsRows(const QByteArray &mppBytes)
{
    QList<ConsRow> rows;
    CompoundFile cf;
    if (!cf.openFromData(mppBytes))
        return rows;
    const QByteArray meta = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndCons"),
                                             QStringLiteral("FixedMeta") });
    const QByteArray data = cf.readStream({ QStringLiteral("   114"), QStringLiteral("TBkndCons"),
                                             QStringLiteral("FixedData") });
    const int count = (meta.size() - 16) / 10;
    for (int loop = 0; loop < count; ++loop) {
        const int metaPos = 16 + loop * 10;
        quint16 dead = 0;
        if (!FieldDecoders::readU16(meta, metaPos, &dead) || dead != 0)
            continue;
        quint32 off = 0;
        if (!FieldDecoders::readU32(meta, metaPos + 4, &off) || off + 12 > quint32(data.size()))
            continue;
        ConsRow row{};
        FieldDecoders::readU32(data, int(off), &row.uid);
        FieldDecoders::readU32(data, int(off) + 4, &row.pred);
        FieldDecoders::readU32(data, int(off) + 8, &row.succ);
        rows.append(row);
    }
    return rows;
}

} // namespace

void TstXmlPredecessorGuard::readerDropsInvalidLinks()
{
    XmlIO io;
    QVERIFY2(io.openFromData(QByteArray(kBrokenLinksXml)), qPrintable(io.errorString()));

    const schedule::Project &p = io.project();

    // The missing-UID, explicit-zero-UID and self-referencing links must all
    // be dropped; only the one valid link (1 -> 2) survives.
    QCOMPARE(p.relations.size(), 1);
    QCOMPARE(p.relations.first().predecessorTaskUid, 1);
    QCOMPARE(p.relations.first().successorTaskUid, 2);
}

void TstXmlPredecessorGuard::writerNeverEmitsBadRow()
{
    // Bypass both readers entirely: hand-build a model with relations no
    // reader would ever produce, to prove the writer's own guard is what
    // keeps them out of the file, not just that upstream readers are clean.
    schedule::Project p;
    p.formatVersion = schedule::Project::FormatVersion::Mpp14;
    p.startDate = QDateTime(QDate(2026, 1, 5), QTime(8, 0));
    p.finishDate = QDateTime(QDate(2026, 2, 27), QTime(17, 0));

    for (int uid = 1; uid <= 4; ++uid) {
        schedule::Task t;
        t.uniqueId = uid;
        t.id = uid;
        t.name = QStringLiteral("Task %1").arg(uid);
        t.start = p.startDate;
        t.finish = p.finishDate;
        p.tasks.append(t);
    }

    schedule::Relation zeroPred;   // invalid: predecessor is task uid 0
    zeroPred.predecessorTaskUid = 0;
    zeroPred.successorTaskUid = 2;
    p.relations.append(zeroPred);

    schedule::Relation selfLink;   // invalid: self-reference
    selfLink.predecessorTaskUid = 2;
    selfLink.successorTaskUid = 2;
    p.relations.append(selfLink);

    schedule::Relation validA;     // valid, uniqueId left at its 0 default
    validA.predecessorTaskUid = 1;
    validA.successorTaskUid = 2;
    p.relations.append(validA);

    schedule::Relation validB;     // valid, uniqueId also left at its 0 default
    validB.predecessorTaskUid = 1;
    validB.successorTaskUid = 3;
    p.relations.append(validB);

    schedule::Relation validC;     // valid, with a real (non-zero) uniqueId
    validC.uniqueId = 5;
    validC.predecessorTaskUid = 1;
    validC.successorTaskUid = 4;
    p.relations.append(validC);

    MppIO mpp;
    mpp.setProject(p);
    const QByteArray bytes = mpp.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(mpp.errorString()));

    const QList<ConsRow> rows = rawConsRows(bytes);

    // Only the three valid relations were written.
    QCOMPARE(rows.size(), 3);
    for (const ConsRow &row : rows) {
        QVERIFY(row.pred != 0);
        QVERIFY(row.succ != 0);
        QVERIFY(row.pred != row.succ);
    }

    // Every row got a distinct id, and the two synthesized for validA/validB
    // (whose uniqueId was left at 0) were seeded above the real uid (5) so
    // they can't collide with it or each other.
    QSet<quint32> uids;
    for (const ConsRow &row : rows)
        uids.insert(row.uid);
    QCOMPARE(uids.size(), rows.size());

    int realCount = 0, synthAboveFive = 0;
    for (const ConsRow &row : rows) {
        if (row.uid == 5)
            ++realCount;
        else if (row.uid > 5)
            ++synthAboveFive;
    }
    QCOMPARE(realCount, 1);
    QCOMPARE(synthAboveFive, 2);
}

void TstXmlPredecessorGuard::endToEndBrokenXmlToMpp()
{
    // Reproduces the exact user-reported path: an .xml with a broken
    // predecessor link, saved as .mpp, must never carry that link into the
    // real file's TBkndCons table.
    XmlIO xml;
    QVERIFY2(xml.openFromData(QByteArray(kBrokenLinksXml)), qPrintable(xml.errorString()));

    schedule::Project p = xml.project();
    p.formatVersion = schedule::Project::FormatVersion::Mpp14;
    p.startDate = QDateTime(QDate(2026, 1, 5), QTime(8, 0));
    p.finishDate = QDateTime(QDate(2026, 2, 27), QTime(17, 0));
    for (schedule::Task &t : p.tasks) {
        t.start = p.startDate;
        t.finish = p.finishDate;
    }

    MppIO mpp;
    mpp.setProject(p);
    const QByteArray bytes = mpp.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(mpp.errorString()));

    const QList<ConsRow> rows = rawConsRows(bytes);

    // Only the one valid link (1 -> 2) reached the file.
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().pred, 1u);
    QCOMPARE(rows.first().succ, 2u);
}

QTEST_MAIN(TstXmlPredecessorGuard)
#include "tst_xml_predecessor_guard.moc"
