// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "ole/compoundfile.h"
#include "serializer/docserializer.h"

#include <QTest>
#include <QtEndian>

using FormatVersion = MppProject::FormatVersion;

class TstVersionDispatch : public QObject
{
    Q_OBJECT
private slots:
    void detectsKnownVersions_data();
    void detectsKnownVersions();
    void createMatchesVersion();
    void unknownVersionHasNoSerializer();
};

static CompoundFile makeContainerWithVersion(quint16 ver)
{
    QByteArray props(2, '\0');
    qToLittleEndian<quint16>(ver, reinterpret_cast<uchar *>(props.data()));
    CompoundFile cf;
    cf.addStream({ QStringLiteral("Project"), QStringLiteral("Props") }, props);
    // Round-trip through real bytes so detection runs on a parsed container.
    CompoundFile parsed;
    parsed.openFromData(cf.toByteArray());
    return parsed;
}

void TstVersionDispatch::detectsKnownVersions_data()
{
    QTest::addColumn<quint16>("raw");
    QTest::addColumn<int>("expected");
    QTest::newRow("mpp12") << quint16(12) << int(FormatVersion::Mpp12);
    QTest::newRow("mpp14") << quint16(14) << int(FormatVersion::Mpp14);
    QTest::newRow("unknown 99") << quint16(99) << int(FormatVersion::Unknown);
}

void TstVersionDispatch::detectsKnownVersions()
{
    QFETCH(quint16, raw);
    QFETCH(int, expected);
    const CompoundFile cf = makeContainerWithVersion(raw);
    QCOMPARE(int(DocSerializer::detectVersion(cf)), expected);
}

void TstVersionDispatch::createMatchesVersion()
{
    auto s12 = DocSerializer::create(FormatVersion::Mpp12);
    QVERIFY(s12);
    QCOMPARE(s12->version(), FormatVersion::Mpp12);

    auto s14 = DocSerializer::create(FormatVersion::Mpp14);
    QVERIFY(s14);
    QCOMPARE(s14->version(), FormatVersion::Mpp14);
}

void TstVersionDispatch::unknownVersionHasNoSerializer()
{
    QVERIFY(!DocSerializer::create(FormatVersion::Unknown));
}

QTEST_APPLESS_MAIN(TstVersionDispatch)
#include "tst_versiondispatch.moc"
