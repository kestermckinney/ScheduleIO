// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef BKNDVARDATA_H
#define BKNDVARDATA_H

#include <QByteArray>
#include <QString>
#include <QVector>

// Reader for a Bknd entity's variable-data pair (VarMeta + Var2Data), ported
// from MPXJ's VarMeta12 / Var2Data (github.com/joniles/mpxj). Layout:
//
//   VarMeta : magic 0xFADFADBA, then a 24-byte header
//             [magic][u32][u32 itemCount][u32][u32][u32 dataSize], then
//             12-byte records: [u32 uniqueID][u32 offset][u16 type][u16 unused].
//   Var2Data: blob pool; a string blob at `offset` is [u32 byteLen][UTF-16LE].
//
// The record `type` is the field's var-data key. Because FieldMap14
// useTypeAsVarDataKey() is true, the key equals (MPP field type id & 0xFFFF),
// which is the MPPTaskField.FIELD_ARRAY index -- e.g. 14 = TaskField.NAME.
//
// Bounds-checked throughout so malformed input cannot crash the parser.
class BkndVarData
{
public:
    // MPP task field var-data keys (MPXJ MPPTaskField.FIELD_ARRAY index).
    static constexpr quint16 FieldTaskName = 14;   // FIELD_ARRAY[14] = TaskField.NAME

    struct Entry {
        quint32 uniqueId = 0;
        QString value;
    };

    bool parse(const QByteArray &varMeta, const QByteArray &var2Data);

    // All length-prefixed UTF-16 strings stored under the given var-data key,
    // paired with their owning unique id, in VarMeta record order.
    QVector<Entry> stringsForType(quint16 type) const;

private:
    struct Record {
        quint32 uniqueId = 0;
        quint32 offset = 0;
        quint16 type = 0;
    };
    QVector<Record> m_records;
    QByteArray m_var2;
};

#endif // BKNDVARDATA_H
