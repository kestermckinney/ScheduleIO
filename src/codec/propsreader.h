// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef PROPSREADER_H
#define PROPSREADER_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

// Reader for the "Props" container used throughout .mpp files (top-level
// "Props14", "   114/Props", per-entity "Props", ...). Reverse-engineered from
// the user's fixtures and verified against the readable entity-name list:
//
//   16-byte header: [u32 byteSize][u32 byteSize][u32][u32]
//   then items:     [u32 dataLen][u32 key][u32 flags][data dataLen bytes]
//
// The key's low 16 bits are the property id; the high bits encode the data
// type. Lookups use the full 32-bit key. Bounds-checked so malformed input
// cannot crash the parser.
//
// This is the foundation for parsing the per-entity field map (which lives in
// "   114/Props" under key 0x03000017 for tasks) -- see DECODING_NOTES.md.
class PropsReader
{
public:
    bool parse(const QByteArray &props);

    bool contains(quint32 key) const { return m_items.contains(key); }
    QByteArray value(quint32 key) const { return m_items.value(key); }
    QString string(quint32 key) const;          // decode value as UTF-16LE
    QList<quint32> keys() const { return m_items.keys(); }
    int count() const { return m_items.size(); }

private:
    QHash<quint32, QByteArray> m_items;
};

#endif // PROPSREADER_H
