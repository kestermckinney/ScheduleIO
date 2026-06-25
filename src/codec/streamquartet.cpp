// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/streamquartet.h"
#include "codec/fielddecoders.h"

#include <QtEndian>

namespace {
void appendU16(QByteArray &b, quint16 v)
{
    char tmp[2];
    qToLittleEndian<quint16>(v, reinterpret_cast<uchar *>(tmp));
    b.append(tmp, 2);
}
void appendU32(QByteArray &b, quint32 v)
{
    char tmp[4];
    qToLittleEndian<quint32>(v, reinterpret_cast<uchar *>(tmp));
    b.append(tmp, 4);
}
} // namespace

StreamQuartet::Streams StreamQuartet::encode() const
{
    Streams s;

    // FixedMeta: [u32 recordCount][u32 recordSize]
    appendU32(s.fixedMeta, static_cast<quint32>(fixedRecords.size()));
    appendU32(s.fixedMeta, static_cast<quint32>(recordSize));

    // FixedData: records back to back.
    for (const QByteArray &rec : fixedRecords)
        s.fixedData.append(rec);

    // Var2Data: [u32 len][bytes] per entry; remember each offset.
    // VarMeta:  [u32 entryCount] then [u32 item][u16 field][u32 offset] per entry.
    appendU32(s.varMeta, static_cast<quint32>(varEntries.size()));
    for (const VarEntry &e : varEntries) {
        const quint32 offset = static_cast<quint32>(s.var2Data.size());
        appendU32(s.var2Data, static_cast<quint32>(e.data.size()));
        s.var2Data.append(e.data);

        appendU32(s.varMeta, e.itemIndex);
        appendU16(s.varMeta, e.fieldType);
        appendU32(s.varMeta, offset);
    }
    return s;
}

StreamQuartet StreamQuartet::decode(const Streams &s, QString *error)
{
    using namespace FieldDecoders;
    StreamQuartet q;
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return StreamQuartet();
    };

    quint32 recordCount = 0, recSize = 0;
    if (!readU32(s.fixedMeta, 0, &recordCount) || !readU32(s.fixedMeta, 4, &recSize))
        return fail(QStringLiteral("FixedMeta too short"));
    q.recordSize = static_cast<int>(recSize);

    if (recSize > 0) {
        const qint64 needed = static_cast<qint64>(recordCount) * recSize;
        if (needed > s.fixedData.size())
            return fail(QStringLiteral("FixedData shorter than FixedMeta declares"));
        for (quint32 i = 0; i < recordCount; ++i)
            q.fixedRecords.append(s.fixedData.mid(i * recSize, recSize));
    }

    quint32 entryCount = 0;
    if (!readU32(s.varMeta, 0, &entryCount))
        return fail(QStringLiteral("VarMeta too short"));
    int off = 4;
    for (quint32 i = 0; i < entryCount; ++i) {
        VarEntry e;
        quint32 dataOffset = 0;
        if (!readU32(s.varMeta, off, &e.itemIndex)
            || !readU16(s.varMeta, off + 4, &e.fieldType)
            || !readU32(s.varMeta, off + 6, &dataOffset))
            return fail(QStringLiteral("VarMeta truncated at entry %1").arg(i));
        off += 10;

        quint32 blobLen = 0;
        if (!readU32(s.var2Data, static_cast<int>(dataOffset), &blobLen))
            return fail(QStringLiteral("Var2Data offset out of range at entry %1").arg(i));
        const int blobStart = static_cast<int>(dataOffset) + 4;
        if (blobStart + static_cast<int>(blobLen) > s.var2Data.size())
            return fail(QStringLiteral("Var2Data blob out of range at entry %1").arg(i));
        e.data = s.var2Data.mid(blobStart, static_cast<int>(blobLen));
        q.varEntries.append(e);
    }

    return q;
}
