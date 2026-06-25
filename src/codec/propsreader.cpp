// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/propsreader.h"

#include <QtEndian>

namespace {
constexpr int kHeaderBytes = 16;
constexpr int kItemHeaderBytes = 12;   // [u32 dataLen][u32 key][u32 flags]
} // namespace

bool PropsReader::parse(const QByteArray &props)
{
    m_items.clear();
    if (props.size() < kHeaderBytes)
        return false;

    const uchar *p = reinterpret_cast<const uchar *>(props.constData());
    int o = kHeaderBytes;
    while (o + kItemHeaderBytes <= props.size()) {
        const quint32 len = qFromLittleEndian<quint32>(p + o);
        const quint32 key = qFromLittleEndian<quint32>(p + o + 4);
        o += kItemHeaderBytes;
        if (len > quint32(props.size() - o))
            break;   // truncated / not a Props stream
        m_items.insert(key, props.mid(o, static_cast<int>(len)));
        o += static_cast<int>(len);
    }
    return !m_items.isEmpty();
}

QString PropsReader::string(quint32 key) const
{
    const QByteArray d = m_items.value(key);
    if (d.size() < 2)
        return QString();
    QString s = QString::fromUtf16(reinterpret_cast<const char16_t *>(d.constData()), d.size() / 2);
    while (s.endsWith(QChar(u'\0')))
        s.chop(1);
    return s;
}
