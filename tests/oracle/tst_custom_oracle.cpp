// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QTest>
#include <QXmlStreamReader>

#ifndef SCHEDULEIO_FIXTURE_DIR
#define SCHEDULEIO_FIXTURE_DIR ""
#endif

// Layer 3 oracle: custom ("extended") task fields decoded from var/fixed data must
// agree with the per-task <ExtendedAttribute> values in the MS Project XML export.
// The XML <FieldID> equals the binary field id (high word entity | low word index),
// which is exactly schedule::CustomField::fieldId, so we match on that.
class TstCustomOracle : public QObject
{
    Q_OBJECT
private slots:
    void customMatchesXml_data();
    void customMatchesXml();

private:
    // uid -> (fieldId -> raw XML value string)
    static QHash<int, QHash<int, QString>> parse(const QString &xmlPath);
    static qint64 isoDur(const QString &s);
};

qint64 TstCustomOracle::isoDur(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("PT(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?"));
    const QRegularExpressionMatch m = re.match(s);
    if (!m.hasMatch())
        return -1;
    return ((m.captured(1).toLongLong() * 3600) + (m.captured(2).toLongLong() * 60)
            + m.captured(3).toLongLong()) * 1000;
}

QHash<int, QHash<int, QString>> TstCustomOracle::parse(const QString &xmlPath)
{
    QHash<int, QHash<int, QString>> out;
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QXmlStreamReader xml(&f);
    bool inTask = false, inExt = false;
    int uid = -1, fieldId = -1;
    QString value;
    bool valueSeen = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView n = xml.name();
            if (n == u"Task") { inTask = true; uid = -1; }
            else if (inTask && n == u"UID" && uid < 0 && !inExt) uid = xml.readElementText().toInt();
            else if (inTask && n == u"ExtendedAttribute") { inExt = true; fieldId = -1; value.clear(); valueSeen = false; }
            else if (inExt && n == u"FieldID") fieldId = xml.readElementText().toInt();
            else if (inExt && n == u"Value") { value = xml.readElementText(); valueSeen = true; }
        } else if (xml.isEndElement()) {
            const QStringView n = xml.name();
            if (n == u"ExtendedAttribute") { if (uid >= 0 && fieldId >= 0 && valueSeen) out[uid].insert(fieldId, value); inExt = false; }
            else if (n == u"Task") inTask = false;
        }
    }
    return out;
}

void TstCustomOracle::customMatchesXml_data()
{
    QTest::addColumn<QString>("mpp");
    QTest::addColumn<QString>("xml");
    const QString dir = QStringLiteral(SCHEDULEIO_FIXTURE_DIR);
    for (const QString &f : QDir(dir).entryList({ QStringLiteral("*.mpp") }, QDir::Files)) {
        const QString xml = QDir(dir).filePath(QFileInfo(f).completeBaseName() + QStringLiteral(".xml"));
        if (QFile::exists(xml))
            QTest::newRow(qPrintable(f)) << QDir(dir).filePath(f) << xml;
    }
}

// Compare a decoded QVariant custom value to its raw XML string representation.
static bool valueMatches(const QVariant &v, const QString &xml)
{
    switch (static_cast<QMetaType::Type>(v.typeId())) {
    case QMetaType::QString:
        return v.toString() == xml;
    case QMetaType::Double:
        return qAbs(v.toDouble() - xml.toDouble()) < 0.05;
    case QMetaType::Bool:
        return (v.toBool() ? 1 : 0) == (xml == QLatin1String("1") || xml.toInt() != 0 ? 1 : 0);
    case QMetaType::LongLong: {   // a duration in ms; XML is ISO-8601
        static const QRegularExpression re(QStringLiteral("PT(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?"));
        const QRegularExpressionMatch m = re.match(xml);
        if (!m.hasMatch()) return false;
        const qint64 ms = ((m.captured(1).toLongLong() * 3600) + (m.captured(2).toLongLong() * 60)
                           + m.captured(3).toLongLong()) * 1000;
        return v.toLongLong() == ms;
    }
    case QMetaType::QDateTime: {
        const QDateTime x = QDateTime::fromString(xml, Qt::ISODate);
        const QDateTime d = v.toDateTime();
        return d.isValid() && x.isValid() && d.date() == x.date() && d.time() == x.time();
    }
    default:
        return false;
    }
}

void TstCustomOracle::customMatchesXml()
{
    if (QDir(QStringLiteral(SCHEDULEIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files).isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));

    const QHash<int, QHash<int, QString>> expected = parse(xml);
    int total = 0;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it)
        total += it.value().size();
    if (total == 0)
        QSKIP("this fixture has no custom (extended) task attributes in its XML export");

    // Decoded: uid -> (fieldId -> value)
    QHash<int, QHash<int, QVariant>> decoded;
    for (const schedule::Task &t : io.project().tasks)
        for (const schedule::CustomField &c : t.customFields)
            decoded[t.uniqueId].insert(c.fieldId, c.value);

    int n = 0, present = 0, ok = 0;
    for (auto uit = expected.constBegin(); uit != expected.constEnd(); ++uit) {
        const auto dmap = decoded.constFind(uit.key());
        for (auto fit = uit.value().constBegin(); fit != uit.value().constEnd(); ++fit) {
            ++n;
            if (dmap == decoded.constEnd())
                continue;
            const auto d = dmap->constFind(fit.key());
            if (d == dmap->constEnd())
                continue;
            ++present;
            if (valueMatches(d.value(), fit.value()))
                ++ok;
        }
    }
    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("custom attrs: %1 in XML, %2 decoded (by id), %3 value-correct")
                             .arg(n).arg(present).arg(ok);
    QVERIFY2(n > 0, "no custom attributes in XML");
    // Of the custom attributes MS Project exported, we must decode and correctly
    // value-match the large majority.
    QVERIFY2(double(ok) / n >= 0.85, "custom field values disagree with XML");
}

QTEST_MAIN(TstCustomOracle)
#include "tst_custom_oracle.moc"
