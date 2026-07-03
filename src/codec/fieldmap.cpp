// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/fieldmap.h"

#include "codec/fielddecoders.h"
#include "codec/propsreader.h"

namespace FieldMap {

QHash<quint16, EntityFieldLoc> entityFieldLocations(const QByteArray &fm, quint16 highWord)
{
    using namespace FieldDecoders;
    QHash<quint16, EntityFieldLoc> out;
    int lastOffset = 0, blockIndex = 0;
    for (int i = 0; i + 28 <= fm.size(); i += 28) {
        quint32 typeValue = 0;
        quint16 dataBlockOffset = 0, category = 0;
        readU32(fm, i + 12, &typeValue);
        readU16(fm, i + 4, &dataBlockOffset);
        readU16(fm, i + 20, &category);
        const bool meta = (category == 0x0B || category == 0x64);
        const bool fixed = (!meta && dataBlockOffset != 0xFFFF);
        int thisBlock = 0;
        if (fixed) {
            if (dataBlockOffset < lastOffset)
                ++blockIndex;
            lastOffset = dataBlockOffset;
            thisBlock = blockIndex;
        }
        if ((typeValue >> 16) != highWord)
            continue;
        EntityFieldLoc &L = out[static_cast<quint16>(typeValue & 0xFFFF)];
        if (fixed) {
            if (L.block < 0) {
                L.block = thisBlock;
                L.offset = dataBlockOffset;
            }
        } else if (!meta) {
            L.var = true;
        }
    }
    return out;
}

QHash<quint16, int> block0FixedOffsets(const QByteArray &fm, quint16 highWord)
{
    using namespace FieldDecoders;
    QHash<quint16, int> out;
    int lastOffset = 0, blockIndex = 0;
    for (int i = 0; i + 28 <= fm.size(); i += 28) {
        quint32 typeValue = 0;
        quint16 dataBlockOffset = 0, category = 0;
        readU32(fm, i + 12, &typeValue);
        readU16(fm, i + 4, &dataBlockOffset);
        readU16(fm, i + 20, &category);
        const bool fixed = (category != 0x0B && category != 0x64 && dataBlockOffset != 0xFFFF);
        if (!fixed)
            continue;
        if (dataBlockOffset < lastOffset)
            ++blockIndex;
        lastOffset = dataBlockOffset;
        if (blockIndex == 0 && (typeValue >> 16) == highWord)
            out.insert(static_cast<quint16>(typeValue & 0xFFFF), dataBlockOffset);
    }
    return out;
}

QByteArray fieldMapBytes(const PropsReader &props, quint32 key1, quint32 key2)
{
    QByteArray b = props.value(key1);
    return b.isEmpty() ? props.value(key2) : b;
}

} // namespace FieldMap
