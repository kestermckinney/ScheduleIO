// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef STREAMQUARTET_H
#define STREAMQUARTET_H

#include <QByteArray>
#include <QList>
#include <QString>

// Models the four-stream quartet that WINPROJ stores per entity sub-storage
// (referenceapp.c:447886-448024):
//   FixedData  - fixed-width records, concatenated.
//   FixedMeta  - record count + record size header.
//   Var2Data   - a pool of variable-length blobs.
//   VarMeta    - an (item, field, offset) directory into Var2Data.
//
// This is a faithful *shape* of the format. The precise on-disk field packing
// is confirmed against fixtures (plan, Layer 3); encode()/decode() are exact
// inverses, which is what the Layer 1 quartet test asserts.
class StreamQuartet
{
public:
    struct VarEntry {
        quint32 itemIndex = 0;
        quint16 fieldType = 0;
        QByteArray data;
        bool operator==(const VarEntry &o) const
        {
            return itemIndex == o.itemIndex && fieldType == o.fieldType && data == o.data;
        }
    };

    struct Streams {
        QByteArray fixedMeta;
        QByteArray varMeta;
        QByteArray fixedData;
        QByteArray var2Data;
    };

    int recordSize = 0;                 // width of each fixed record, in bytes
    QList<QByteArray> fixedRecords;     // each must be exactly recordSize bytes
    QList<VarEntry> varEntries;

    Streams encode() const;
    static StreamQuartet decode(const Streams &s, QString *error = nullptr);

    bool operator==(const StreamQuartet &o) const
    {
        return recordSize == o.recordSize
            && fixedRecords == o.fixedRecords
            && varEntries == o.varEntries;
    }
};

#endif // STREAMQUARTET_H
