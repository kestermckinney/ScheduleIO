// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
// Throwaway: locate the first field that does not survive an XmlIO round trip.

#include "xmlio.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

static QTextStream out(stdout);

template <typename T, typename F>
void diffList(const char *label, const QList<T> &a, const QList<T> &b, F describe)
{
    if (a.size() != b.size()) {
        out << label << ": size " << a.size() << " vs " << b.size() << "\n";
        return;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (!(a[i] == b[i])) {
            out << label << "[" << i << "] differs: " << describe(a[i]) << " | " << describe(b[i]) << "\n";
        }
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString path = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("tests/fixtures/Example Template.xml");
    XmlIO a;
    if (!a.open(path)) { out << "open A failed: " << a.errorString() << "\n"; return 1; }
    const QByteArray written = a.saveToData();
    if (argc > 2) {
        QFile f(QString::fromLocal8Bit(argv[2]));
        if (f.open(QIODevice::WriteOnly)) { f.write(written); f.close(); }
    }
    XmlIO b;
    if (!b.openFromData(written)) { out << "open B failed: " << b.errorString() << "\n"; return 1; }

    const MppProject &pa = a.project();
    const MppProject &pb = b.project();
    out << "tasks " << pa.tasks.size() << "/" << pb.tasks.size()
        << " res " << pa.resources.size() << "/" << pb.resources.size()
        << " asn " << pa.assignments.size() << "/" << pb.assignments.size()
        << " cal " << pa.calendars.size() << "/" << pb.calendars.size()
        << " rel " << pa.relations.size() << "/" << pb.relations.size() << "\n";

    diffList("task", pa.tasks, pb.tasks, [](const MppTask &t) {
        return QStringLiteral("uid=%1 name=%2 cf=%3 base=%4").arg(t.uniqueId).arg(t.name)
            .arg(t.customFields.size()).arg(t.baselines.size());
    });
    diffList("resource", pa.resources, pb.resources, [](const MppResource &r) {
        return QStringLiteral("uid=%1 name=%2 cf=%3 rates=%4").arg(r.uniqueId).arg(r.name)
            .arg(r.customFields.size()).arg(r.costRates.size());
    });
    diffList("assignment", pa.assignments, pb.assignments, [](const MppAssignment &x) {
        return QStringLiteral("uid=%1 cf=%2").arg(x.uniqueId).arg(x.customFields.size());
    });
    diffList("calendar", pa.calendars, pb.calendars, [](const MppCalendar &c) {
        return QStringLiteral("uid=%1 name=%2 ex=%3").arg(c.uniqueId).arg(c.name).arg(c.exceptions.size());
    });
    diffList("relation", pa.relations, pb.relations, [](const MppRelation &r) {
        return QStringLiteral("p=%1 s=%2 t=%3 lag=%4").arg(r.predecessorTaskUid)
            .arg(r.successorTaskUid).arg(r.type).arg(r.lagMillis);
    });

    // Drill into custom fields of the first differing task.
    for (int i = 0; i < pa.tasks.size() && i < pb.tasks.size(); ++i) {
        const MppTask &ta = pa.tasks[i];
        const MppTask &tb = pb.tasks[i];
        if (ta == tb) continue;
        out << "first diff task uid=" << ta.uniqueId << "\n";
        for (int j = 0; j < ta.customFields.size() && j < tb.customFields.size(); ++j) {
            const MppCustomField &ca = ta.customFields[j];
            const MppCustomField &cb = tb.customFields[j];
            if (!(ca == cb))
                out << "  cf id=" << ca.fieldId << " name=" << ca.name
                    << " va=[" << ca.value.toString() << "](" << ca.value.typeName() << ")"
                    << " vb=[" << cb.value.toString() << "](" << cb.value.typeName() << ")\n";
        }
        break;
    }
    out.flush();
    return 0;
}
