// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"
#include "xmlio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTest>

#include "fixtureutils.h"

// The whole point of XmlIO is that it shares MppIO's in-memory model. This test
// loads the same project both ways -- the .mpp via MppIO and the .xml via XmlIO --
// and checks the two schedule::Project structures agree on the data they both decode.
// That is only possible because they populate the very same types.
class TstXmlMppCrosscheck : public QObject
{
    Q_OBJECT
private slots:
    void interop_data();
    void interop();
};

void TstXmlMppCrosscheck::interop_data()
{
    QTest::addColumn<QString>("mpp");
    QTest::addColumn<QString>("xml");
    for (const QString &mpp : fixtures::mppFiles()) {
        const QString xml = fixtures::xmlSibling(mpp);
        if (!xml.isEmpty())
            QTest::newRow(qPrintable(fixtures::label(mpp))) << mpp << xml;
    }
}

void TstXmlMppCrosscheck::interop()
{
    if (fixtures::mppFiles().isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    MppIO bin;
    QVERIFY2(bin.open(mpp), qPrintable(bin.errorString()));
    XmlIO text;
    QVERIFY2(text.open(xml), qPrintable(text.errorString()));

    const schedule::Project &fromMpp = bin.project();
    const schedule::Project &fromXml = text.project();

    // Project status date: the .mpp binary props key (STATUS_DATE = 0x02400045) and the
    // MSPDI <StatusDate> element must decode to the same day.
    if (fromXml.statusDate.isValid()) {
        QVERIFY2(fromMpp.statusDate.isValid(), "status date missing from the .mpp decode");
        QCOMPARE(fromMpp.statusDate.date(), fromXml.statusDate.date());
    }

    // Both decoders populate the same model type -- prove interop on task names.
    QHash<int, QString> mppNames;
    for (const schedule::Task &t : fromMpp.tasks)
        if (!t.name.isEmpty())
            mppNames.insert(t.uniqueId, t.name);

    QHash<int, QString> xmlNames;
    for (const schedule::Task &t : fromXml.tasks)
        if (!t.name.isEmpty())
            xmlNames.insert(t.uniqueId, t.name);

    if (xmlNames.isEmpty())
        QSKIP("this fixture has no named tasks in its XML export");

    // Every task the XML export names must be present, by UID, with the same name
    // in the model decoded from the binary file.
    int matched = 0, total = 0;
    for (auto it = xmlNames.constBegin(); it != xmlNames.constEnd(); ++it) {
        if (!mppNames.contains(it.key()))
            continue;   // the .mpp can hold extra/blank rows; only check shared UIDs
        ++total;
        if (mppNames.value(it.key()) == it.value())
            ++matched;
    }
    QVERIFY2(total > 0, "no task UIDs in common between .mpp and .xml");
    QCOMPARE(matched, total);

    // Task Information fields: the .mpp decode (bit flags + field-map fields) must
    // agree with the XML export per shared task UID.
    QHash<int, const schedule::Task *> mppByUid;
    for (const schedule::Task &t : fromMpp.tasks)
        mppByUid.insert(t.uniqueId, &t);
    int fieldChecks = 0;
    for (const schedule::Task &x : fromXml.tasks) {
        const schedule::Task *m = mppByUid.value(x.uniqueId, nullptr);
        if (!m)
            continue;
        ++fieldChecks;
        QVERIFY2(m->manual == x.manual,
                 qPrintable(QStringLiteral("uid %1 manual: mpp=%2 xml=%3")
                                .arg(x.uniqueId).arg(m->manual).arg(x.manual)));
        QVERIFY2(m->effortDriven == x.effortDriven,
                 qPrintable(QStringLiteral("uid %1 effortDriven: mpp=%2 xml=%3")
                                .arg(x.uniqueId).arg(m->effortDriven).arg(x.effortDriven)));
        QVERIFY2(m->taskType == x.taskType,
                 qPrintable(QStringLiteral("uid %1 taskType: mpp=%2 xml=%3")
                                .arg(x.uniqueId).arg(m->taskType).arg(x.taskType)));
        QVERIFY2(m->priority == x.priority,
                 qPrintable(QStringLiteral("uid %1 priority: mpp=%2 xml=%3")
                                .arg(x.uniqueId).arg(m->priority).arg(x.priority)));
        QVERIFY2(m->durationFormat == x.durationFormat,
                 qPrintable(QStringLiteral("uid %1 durationFormat: mpp=%2 xml=%3")
                                .arg(x.uniqueId).arg(m->durationFormat).arg(x.durationFormat)));
    }
    QVERIFY2(fieldChecks > 0, "no shared task UIDs for the task-info field check");

    // Resource kinds use different numeric encodings in COM, MSPDI, and the
    // native row metadata. Both readers must nevertheless produce the same
    // model value and retain a material resource's unit label.
    QHash<int, const schedule::Resource *> mppResources;
    for (const schedule::Resource &resource : fromMpp.resources)
        mppResources.insert(resource.uniqueId, &resource);
    int resourceChecks = 0;
    for (const schedule::Resource &resource : fromXml.resources) {
        const schedule::Resource *native = mppResources.value(resource.uniqueId, nullptr);
        if (!native)
            continue;
        ++resourceChecks;
        QCOMPARE(native->type, resource.type);
        QCOMPARE(native->materialLabel, resource.materialLabel);
    }
    Q_UNUSED(resourceChecks);

    // Assignment planned/actual work is stored in native VarData as cumulative
    // work and cumulative working-time records. Compare its decoded daily
    // distribution with Project's own MSPDI export, not just aggregate Work.
    auto dailyWork = [](const schedule::Project &project) {
        QHash<QString, qint64> result;
        for (const schedule::Assignment &assignment : project.assignments) {
            for (const schedule::TimephasedValue &value : assignment.timephasedValues) {
                if (value.type != schedule::TimephasedValue::RemainingWork
                    && value.type != schedule::TimephasedValue::ActualWork)
                    continue;
                const qint64 amount = value.durationMillis();
                if (amount == 0 || !value.start.isValid())
                    continue;
                const QString key = QStringLiteral("%1|%2|%3")
                    .arg(assignment.uniqueId).arg(value.type)
                    .arg(value.start.date().toString(Qt::ISODate));
                result[key] += amount;
            }
        }
        return result;
    };
    // The generated resource and progress fixtures use ordinary working-time
    // assignments and are the byte-level oracle for fields 49/50. Older,
    // externally sourced fixtures include elapsed-duration and calendar
    // layouts that are outside this codec's current scope.
    const QString fixtureName = QFileInfo(mpp).fileName();
    if (fixtureName == QStringLiteral("04_resources_assignments.mpp")
        || fixtureName == QStringLiteral("07_baseline_progress.mpp")) {
        const QHash<QString, qint64> xmlDaily = dailyWork(fromXml);
        const QHash<QString, qint64> mppDaily = dailyWork(fromMpp);
        QVERIFY(!xmlDaily.isEmpty());
        for (auto it = xmlDaily.constBegin(); it != xmlDaily.constEnd(); ++it)
            QCOMPARE(mppDaily.value(it.key()), it.value());
    }
}

QTEST_MAIN(TstXmlMppCrosscheck)
#include "tst_xml_mpp_crosscheck.moc"
