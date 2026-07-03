// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTest>
#include <QXmlStreamReader>

#ifndef SCHEDULEIO_FIXTURE_DIR
#define SCHEDULEIO_FIXTURE_DIR ""
#endif

// Layer 3 oracle: assignment scheduling fields decoded from the binary .mpp
// (units, work, actual/remaining work, start/finish, delay) must agree with
// the MS Project XML export of the same file.
class TstAssignmentOracle : public QObject
{
    Q_OBJECT
private slots:
    void assignmentsMatchXml_data();
    void assignmentsMatchXml();
};

namespace {

struct XmlAssn {
    double units = 1.0;
    qint64 work = 0, actualWork = 0, remainingWork = 0, delay = 0;
    QDateTime start, finish;
};

// "PT768H0M0S" -> ms (same grammar the XmlIO reader accepts).
qint64 isoDurationMs(const QString &s)
{
    static const QRegularExpression re(
        QStringLiteral("^(-?)P(?:(\\d+)D)?T(\\d+)H(\\d+)M(\\d+(?:\\.\\d+)?)S$"));
    const QRegularExpressionMatch m = re.match(s.trimmed());
    if (!m.hasMatch())
        return 0;
    const qint64 sign = m.captured(1) == QLatin1String("-") ? -1 : 1;
    return sign * (m.captured(2).toLongLong() * 86400000LL
                   + m.captured(3).toLongLong() * 3600000LL
                   + m.captured(4).toLongLong() * 60000LL
                   + qint64(m.captured(5).toDouble() * 1000.0));
}

} // namespace

void TstAssignmentOracle::assignmentsMatchXml_data()
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

void TstAssignmentOracle::assignmentsMatchXml()
{
    if (QDir(QStringLiteral(SCHEDULEIO_FIXTURE_DIR))
            .entryList({ QStringLiteral("*.mpp") }, QDir::Files).isEmpty())
        QSKIP("no .mpp/.xml fixture pairs present");

    QFETCH(QString, mpp);
    QFETCH(QString, xml);

    // --- XML: scheduling fields per assignment UID ---
    QHash<int, XmlAssn> xmlAssn;
    {
        QFile f(xml);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QXmlStreamReader r(&f);
        bool inAssn = false;
        int uid = -1;
        XmlAssn cur;
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement()) {
                const QStringView n = r.name();
                if (n == u"Assignment") { inAssn = true; uid = -1; cur = XmlAssn(); }
                else if (inAssn && (n == u"Baseline" || n == u"TimephasedData"))
                    r.skipCurrentElement();   // their Start/Finish/Work are NOT the assignment's
                else if (inAssn && n == u"UID" && uid < 0) uid = r.readElementText().toInt();
                else if (inAssn && n == u"Units") cur.units = r.readElementText().toDouble();
                else if (inAssn && n == u"Work") cur.work = isoDurationMs(r.readElementText());
                else if (inAssn && n == u"ActualWork") cur.actualWork = isoDurationMs(r.readElementText());
                else if (inAssn && n == u"RemainingWork") cur.remainingWork = isoDurationMs(r.readElementText());
                else if (inAssn && n == u"Delay") cur.delay = r.readElementText().toLongLong() * 6000;
                else if (inAssn && n == u"Start") cur.start = QDateTime::fromString(r.readElementText(), Qt::ISODate);
                else if (inAssn && n == u"Finish") cur.finish = QDateTime::fromString(r.readElementText(), Qt::ISODate);
            } else if (r.isEndElement() && r.name() == u"Assignment") {
                if (uid >= 0)
                    xmlAssn.insert(uid, cur);
                inAssn = false;
            }
        }
    }
    if (xmlAssn.isEmpty())
        QSKIP("fixture has no assignments");

    MppIO io;
    QVERIFY2(io.open(mpp), qPrintable(io.errorString()));
    const schedule::Project &p = io.project();

    // Compare wall-clock date components (decoded values are UTC).
    auto sameStamp = [](const QDateTime &dec, const QDateTime &oracle) {
        if (!oracle.isValid())
            return true;   // XML omitted the field; nothing to check
        return dec.isValid() && dec.date() == oracle.date() && dec.time() == oracle.time();
    };

    int total = 0, unitsOk = 0, workOk = 0, actualOk = 0, remainingOk = 0,
        delayOk = 0, startOk = 0, finishOk = 0;
    for (const schedule::Assignment &a : p.assignments) {
        const auto it = xmlAssn.constFind(a.uniqueId);
        if (it == xmlAssn.constEnd())
            continue;
        ++total;
        unitsOk     += qAbs(a.units - it->units) < 0.001;
        workOk      += a.workMillis == it->work;
        actualOk    += a.actualWorkMillis == it->actualWork;
        remainingOk += a.remainingWorkMillis == it->remainingWork;
        delayOk     += a.delayMillis == it->delay;
        startOk     += sameStamp(a.start, it->start);
        finishOk    += sameStamp(a.finish, it->finish);
    }

    qInfo().noquote() << QFileInfo(mpp).fileName()
                      << QStringLiteral("matched %1  units %2 work %3 actual %4 remaining %5 delay %6 start %7 finish %8")
                             .arg(total).arg(unitsOk).arg(workOk).arg(actualOk)
                             .arg(remainingOk).arg(delayOk).arg(startOk).arg(finishOk);

    QVERIFY2(total > 0, "no decoded assignment matched an XML assignment UID");
    const auto atLeast = [&](int ok, const char *what) {
        QVERIFY2(double(ok) / total >= 0.98,
                 qPrintable(QStringLiteral("%1: %2/%3 disagree with XML").arg(what).arg(total - ok).arg(total)));
    };
    atLeast(unitsOk, "units");
    atLeast(workOk, "work");
    atLeast(actualOk, "actual work");
    atLeast(remainingOk, "remaining work");
    atLeast(delayOk, "delay");
    atLeast(startOk, "start");
    atLeast(finishOk, "finish");
}

QTEST_MAIN(TstAssignmentOracle)
#include "tst_assignment_oracle.moc"
