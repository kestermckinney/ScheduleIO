// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "serializer/mpp14manifest.h"

#include "codec/propsreader.h"
#include "ole/compoundfile.h"

#include <QtEndian>

namespace {

struct Rowset {
    const char *parent;
    const char *storage;
    quint16 containerId;
};

// Container IDs are native Project backend type IDs, not list ordinals.  They
// were recovered from the corresponding property-key families in real Project
// files and from the backend container tables in ReferenceApp/referenceapp.c.
constexpr Rowset kRowsets[] = {
    { "   114", "TBkndTask",              1 },
    { "   114", "TBkndRsc",               2 },
    { "   114", "TBkndCal",               3 },
    { "   114", "TBkndAssn",              4 },
    { "   114", "TBkndCons",              5 },
    { "   114", "TBkndOutlCode",          8 },
    { "   114", "TBkndAttachment",       14 },
    { "   114", "TBkndChecklistItem",    15 },
    { "   114", "TBkndConversation",     16 },
    { "   114", "TBkndLabel",            17 },
    { "   114", "TBkndLabelAssociation", 18 },
    { "   214", "CV_iew",                 1 },
    { "   214", "CFilter",                2 },
    { "   214", "CTable",                 3 },
    { "   214", "CReport",                4 },
    { "   214", "CEdl",                   7 },
    { "   214", "CMap",                   9 },
    { "   214", "CVba",                  10 },
    { "   214", "CGrouping",             11 },
    { "   214", "CDrawing",              13 },
};

quint32 u32(const QByteArray &data, int offset, bool *ok = nullptr)
{
    const bool valid = offset >= 0 && offset + 4 <= data.size();
    if (ok)
        *ok = valid;
    return valid ? qFromLittleEndian<quint32>(
                       reinterpret_cast<const uchar *>(data.constData() + offset))
                 : 0;
}

bool propsU32(const PropsReader &props, quint32 key, quint32 *value)
{
    const QByteArray data = props.value(key);
    if (data.size() < 4)
        return false;
    *value = u32(data, 0);
    return true;
}

bool patchPropsU32(QByteArray &props, quint32 key, quint32 value)
{
    int offset = 16;
    while (offset + 12 <= props.size()) {
        const quint32 length = u32(props, offset);
        const quint32 itemKey = u32(props, offset + 4);
        offset += 12;
        if (length > quint32(props.size() - offset))
            return false;
        if (itemKey == key && length >= 4) {
            qToLittleEndian<quint32>(
                value, reinterpret_cast<uchar *>(props.data() + offset));
            return true;
        }
        offset += int(length);
    }
    return false;
}

QString label(const Rowset &rowset)
{
    return QString::fromLatin1(rowset.parent) + QLatin1Char('/')
        + QString::fromLatin1(rowset.storage);
}

void compareHeaderSize(QStringList *issues, const QString &rowsetLabel,
                       const QByteArray &meta, int sizeOffset,
                       const QByteArray &data, const QString &streamName)
{
    bool ok = false;
    const quint32 declared = u32(meta, sizeOffset, &ok);
    if (!ok) {
        issues->append(QStringLiteral("%1/%2 header is truncated")
                           .arg(rowsetLabel, streamName));
    } else if (declared != quint32(data.size())) {
        issues->append(QStringLiteral("%1/%2 declares %3 bytes; stream has %4")
                           .arg(rowsetLabel, streamName)
                           .arg(declared).arg(data.size()));
    }
}

} // namespace

namespace Mpp14Manifest {

QStringList audit(const CompoundFile &cf)
{
    QStringList issues;
    QHash<QString, PropsReader> parentProps;

    for (const Rowset &rowset : kRowsets) {
        const QString parent = QString::fromLatin1(rowset.parent);
        const QString storage = QString::fromLatin1(rowset.storage);
        const QStringList base = { parent, storage };
        if (!cf.hasStorage(base))
            continue;

        if (!parentProps.contains(parent)) {
            PropsReader props;
            if (!props.parse(cf.readStream({ parent, QStringLiteral("Props") }))) {
                issues.append(QStringLiteral("%1/Props is missing or malformed").arg(parent));
                parentProps.insert(parent, PropsReader());
            } else {
                parentProps.insert(parent, props);
            }
        }
        const PropsReader &props = parentProps[parent];
        const QString rowsetLabel = label(rowset);
        const QStringList fixedMetaPath = base + QStringList{ QStringLiteral("FixedMeta") };
        if (!cf.hasStream(fixedMetaPath)) {
            quint32 declaredRows = 0;
            quint32 declaredVar2Size = 0;
            propsU32(props, 0x01000000u | rowset.containerId, &declaredRows);
            propsU32(props, 0x00010000u | rowset.containerId, &declaredVar2Size);
            if (declaredRows != 0 || declaredVar2Size != 0)
                issues.append(QStringLiteral("%1 has declarations but no rowset streams")
                                  .arg(rowsetLabel));
            continue;
        }
        const QByteArray fixedMeta = cf.readStream(base + QStringList{ QStringLiteral("FixedMeta") });
        const QByteArray fixedData = cf.readStream(base + QStringList{ QStringLiteral("FixedData") });
        const QByteArray fixed2Meta = cf.readStream(base + QStringList{ QStringLiteral("Fixed2Meta") });
        const QByteArray fixed2Data = cf.readStream(base + QStringList{ QStringLiteral("Fixed2Data") });
        const QByteArray varMeta = cf.readStream(base + QStringList{ QStringLiteral("VarMeta") });
        const QByteArray var2Data = cf.readStream(base + QStringList{ QStringLiteral("Var2Data") });

        bool countOk = false;
        const quint32 actualRows = u32(fixedMeta, 8, &countOk);
        quint32 declaredRows = 0;
        const quint32 countKey = 0x01000000u | rowset.containerId;
        if (!countOk) {
            issues.append(QStringLiteral("%1/FixedMeta header is truncated").arg(rowsetLabel));
        } else if (!propsU32(props, countKey, &declaredRows)) {
            issues.append(QStringLiteral("%1 parent row-count declaration is missing").arg(rowsetLabel));
        } else if (declaredRows != actualRows) {
            issues.append(QStringLiteral("%1 declares %2 rows; FixedMeta has %3")
                              .arg(rowsetLabel).arg(declaredRows).arg(actualRows));
        }

        quint32 declaredVar2Size = 0;
        const quint32 varSizeKey = 0x00010000u | rowset.containerId;
        if (!propsU32(props, varSizeKey, &declaredVar2Size)) {
            issues.append(QStringLiteral("%1 parent Var2Data declaration is missing")
                              .arg(rowsetLabel));
        } else if (declaredVar2Size != quint32(var2Data.size())) {
            issues.append(QStringLiteral("%1 declares %2 Var2Data bytes; stream has %3")
                              .arg(rowsetLabel).arg(declaredVar2Size).arg(var2Data.size()));
        }

        compareHeaderSize(&issues, rowsetLabel, fixedMeta, 12, fixedData,
                          QStringLiteral("FixedMeta"));
        if (cf.hasStream(base + QStringList{ QStringLiteral("Fixed2Meta") }))
            compareHeaderSize(&issues, rowsetLabel, fixed2Meta, 12, fixed2Data,
                              QStringLiteral("Fixed2Meta"));
        if (cf.hasStream(base + QStringList{ QStringLiteral("VarMeta") }))
            compareHeaderSize(&issues, rowsetLabel, varMeta, 20, var2Data,
                              QStringLiteral("VarMeta"));
    }
    return issues;
}

bool reconcile(CompoundFile &cf, QString *error)
{
    const QString parents[] = { QStringLiteral("   114"), QStringLiteral("   214") };
    for (const QString &parent : parents) {
        QByteArray props = cf.readStream({ parent, QStringLiteral("Props") });
        PropsReader parsed;
        if (!parsed.parse(props)) {
            if (error)
                *error = QStringLiteral("%1/Props is missing or malformed").arg(parent);
            return false;
        }

        for (const Rowset &rowset : kRowsets) {
            if (parent != QString::fromLatin1(rowset.parent))
                continue;
            const QString storage = QString::fromLatin1(rowset.storage);
            const QStringList base = { parent, storage };
            if (!cf.hasStream(base + QStringList{ QStringLiteral("FixedMeta") }))
                continue;

            bool ok = false;
            const quint32 rows = u32(
                cf.readStream(base + QStringList{ QStringLiteral("FixedMeta") }), 8, &ok);
            if (!ok || !patchPropsU32(props, 0x01000000u | rowset.containerId, rows)
                || !patchPropsU32(props, 0x00010000u | rowset.containerId,
                                  quint32(cf.readStream(base + QStringList{
                                      QStringLiteral("Var2Data") }).size()))) {
                if (error)
                    *error = QStringLiteral("cannot reconcile manifest for %1")
                                 .arg(label(rowset));
                return false;
            }
        }
        cf.addStream({ parent, QStringLiteral("Props") }, props);
    }
    return true;
}

} // namespace Mpp14Manifest
