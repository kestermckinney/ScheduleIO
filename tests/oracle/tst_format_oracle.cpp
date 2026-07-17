// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include "fixtureutils.h"

// Layer 3 oracle for per-cell text formatting. Some fixture folders carry a
// manifest.json recording exactly what Format>Font styling was applied to
// which row (the MSPDI XML export never contains presentation data, so the
// manifest is the only ground truth). Every manifest row must decode to the
// matching Task::rowFormat. Font family/size are in the manifests but are
// deliberately not modelled (see TextStyle) and therefore not checked.
class TstFormatOracle : public QObject
{
    Q_OBJECT
private slots:
    void formatsMatchManifest_data();
    void formatsMatchManifest();
};

namespace {

// COLORREF (R + G*256 + B*65536) -> model 0xRRGGBB.
qint32 colorrefToRgb(qint64 colorref)
{
    const int r = int(colorref & 0xFF);
    const int g = int((colorref >> 8) & 0xFF);
    const int b = int((colorref >> 16) & 0xFF);
    return qint32((r << 16) | (g << 8) | b);
}

} // namespace

void TstFormatOracle::formatsMatchManifest_data()
{
    QTest::addColumn<QString>("mpp");
    QTest::addColumn<QString>("manifest");
    for (const QString &mpp : fixtures::mppFiles()) {
        const QString manifest = QFileInfo(mpp).dir().filePath(QStringLiteral("manifest.json"));
        if (QFile::exists(manifest))
            QTest::newRow(qPrintable(fixtures::label(mpp))) << mpp << manifest;
    }
}

void TstFormatOracle::formatsMatchManifest()
{
    if (fixtures::mppFiles().isEmpty())
        QSKIP("no .mpp fixtures present");
    QFETCH(QString, mpp);
    QFETCH(QString, manifest);

    QFile mf(manifest);
    QVERIFY(mf.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(mf.readAll()).object();
    const QJsonArray rows = root.value(QStringLiteral("rows")).toArray();
    QVERIFY2(!rows.isEmpty(), "manifest has no rows");

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));

    QHash<QString, schedule::TextStyle> byName;
    for (const schedule::Task &t : io.project().tasks)
        byName.insert(t.name, t.rowFormat);

    int checked = 0;
    for (const QJsonValue &v : rows) {
        const QJsonObject r = v.toObject();
        const QString name = r.value(QStringLiteral("taskName")).toString();
        QVERIFY2(byName.contains(name), qPrintable(QStringLiteral("task '%1' not decoded").arg(name)));
        const schedule::TextStyle &s = byName.value(name);

        const auto fail = [&](const char *what, const QString &detail) {
            return qPrintable(QStringLiteral("row %1 '%2' %3: %4")
                                  .arg(r.value(QStringLiteral("row")).toInt())
                                  .arg(name, QLatin1String(what), detail));
        };
        QVERIFY2(s.bold == r.value(QStringLiteral("bold")).toBool(), fail("bold", QString::number(s.bold)));
        QVERIFY2(s.italic == r.value(QStringLiteral("italic")).toBool(), fail("italic", QString::number(s.italic)));
        QVERIFY2(s.underline == r.value(QStringLiteral("underline")).toBool(),
                 fail("underline", QString::number(s.underline)));
        QVERIFY2(s.strikethrough == r.value(QStringLiteral("strikethrough")).toBool(),
                 fail("strikethrough", QString::number(s.strikethrough)));

        // Text colour: the generator always passes an explicit COLORREF, so
        // the file stores an explicit RGB (0 = explicit black, not Automatic).
        const qint32 wantColor = colorrefToRgb(qint64(r.value(QStringLiteral("colorCOLORREF")).toDouble()));
        QVERIFY2(s.color == wantColor,
                 fail("color", QStringLiteral("decoded %1 want %2").arg(s.color, 6, 16).arg(wantColor, 6, 16)));

        // Cell background, when the manifest specifies one. Pattern semantics
        // verified against MS Project's own rendering of 10_cell_background:
        // 0 = transparent, 1 = solid, 2 = light dotted, 4 = heavy dotted
        // (matches MPXJ's BackgroundPattern enum).
        const QJsonValue back = r.value(QStringLiteral("cellBackgroundCOLORREF"));
        const int wantPattern = r.value(QStringLiteral("pattern")).toInt();
        if (!back.isNull() && !back.isUndefined()) {
            QVERIFY2(s.backPattern == wantPattern,
                     fail("backPattern", QStringLiteral("decoded %1 want %2").arg(s.backPattern).arg(wantPattern)));
            if (wantPattern >= 1) {
                const qint32 wantBack = colorrefToRgb(qint64(back.toDouble()));
                QVERIFY2(s.backColor == wantBack,
                         fail("backColor", QStringLiteral("decoded %1 want %2").arg(s.backColor, 6, 16).arg(wantBack, 6, 16)));
            }
        }
        ++checked;
    }
    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("verified %1 manifest rows").arg(checked);
}

QTEST_MAIN(TstFormatOracle)
#include "tst_format_oracle.moc"
