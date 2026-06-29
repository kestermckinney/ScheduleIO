// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/bkndvardata.h"

#include <QtEndian>

namespace {
constexpr quint32 kVarMetaMagic = 0xFADFADBA;
constexpr int kHeaderBytes = 24;   // magic + 4 u32 + dataSize
constexpr int kRecordBytes = 12;   // [u32 uid][u32 offset][u16 type][u16 unused]
} // namespace

bool BkndVarData::parse(const QByteArray &varMeta, const QByteArray &var2Data)
{
    m_records.clear();
    m_var2 = var2Data;

    if (varMeta.size() < kHeaderBytes)
        return false;
    const uchar *p = reinterpret_cast<const uchar *>(varMeta.constData());
    if (qFromLittleEndian<quint32>(p) != kVarMetaMagic)
        return false;

    for (int o = kHeaderBytes; o + kRecordBytes <= varMeta.size(); o += kRecordBytes) {
        Record r;
        r.uniqueId = qFromLittleEndian<quint32>(p + o);
        r.offset   = qFromLittleEndian<quint32>(p + o + 4);
        r.type     = qFromLittleEndian<quint16>(p + o + 8);
        m_records.append(r);
    }
    return true;
}

QVector<BkndVarData::Entry> BkndVarData::stringsForType(quint16 type) const
{
    QVector<Entry> out;
    const uchar *v = reinterpret_cast<const uchar *>(m_var2.constData());
    const quint32 size = static_cast<quint32>(m_var2.size());

    for (const Record &r : m_records) {
        if (r.type != type)
            continue;
        if (r.offset + 4 > size)
            continue;
        const quint32 len = qFromLittleEndian<quint32>(v + r.offset);
        if (len < 2 || r.offset + 4 + len > size)
            continue;
        QString s = QString::fromUtf16(
            reinterpret_cast<const char16_t *>(v + r.offset + 4), static_cast<int>(len / 2));
        while (s.endsWith(QChar(u'\0')))   // stored length includes the NUL terminator
            s.chop(1);
        out.append({ r.uniqueId, s });
    }
    return out;
}

QByteArray BkndVarData::blobFor(quint32 uniqueId, quint16 type) const
{
    const uchar *v = reinterpret_cast<const uchar *>(m_var2.constData());
    const quint32 size = static_cast<quint32>(m_var2.size());
    for (const Record &r : m_records) {
        if (r.type != type || r.uniqueId != uniqueId)
            continue;
        if (r.offset + 4 > size)
            return QByteArray();
        const quint32 len = qFromLittleEndian<quint32>(v + r.offset);
        if (len == 0 || r.offset + 4 + len > size)
            return QByteArray();
        return m_var2.mid(static_cast<int>(r.offset + 4), static_cast<int>(len));
    }
    return QByteArray();
}
