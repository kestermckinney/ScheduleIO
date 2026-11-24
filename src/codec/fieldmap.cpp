// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/fieldmap.h"
#include "codec/mppfieldids.h"

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
        } else if (category == 0x0B && L.metaOffset < 0) {
            // Meta bits: u32 mask @0, offset @8 counted from item byte 2 (so
            // MILESTONE's 8 is FixedMeta item byte 10, Flag1's 22 byte 24).
            quint16 metaOffset = 0;
            quint32 mask = 0;
            readU16(fm, i + 8, &metaOffset);
            readU32(fm, i, &mask);
            if (mask != 0) {
                L.metaOffset = int(metaOffset) + 2;
                L.metaMask = mask;
            }
        }
    }
    // A task custom flag (Flag1-20) is the FixedMeta bit at its field-map
    // position, the same bitmap that marks which var fields a row carries
    // (Project 2016, 2026-11-17: Flag1 item byte 24 bit 1 = position 129,
    // Flag11 byte 33 bit 1 = 201, Flag20 byte 34 bit 2 = 210). The masks the
    // map gives Flag11-20 point into Flag1-10's word instead -- Flag11's 0x800
    // is Number1's presence bit -- so they are not used.
    if (highWord == MppFieldIds::kTaskHigh) {
        const QHash<quint16, int> ordinals = presenceBitOrdinals(fm, highWord);
        for (const MppFieldIds::CustomFieldDef &d : MppFieldIds::taskCustomFields()) {
            if (d.kind != MppFieldIds::FieldKind::Bool || !ordinals.contains(d.index))
                continue;
            const int ordinal = ordinals.value(d.index);
            EntityFieldLoc &L = out[d.index];
            L.metaOffset = 8 + ordinal / 8;
            L.metaMask = 1u << (ordinal % 8);
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

QHash<quint16, int> presenceBitOrdinals(const QByteArray &fm, quint16 highWord)
{
    using namespace FieldDecoders;
    QHash<quint16, int> out;
    int ordinal = 0;
    for (int i = 0; i + 28 <= fm.size(); i += 28) {
        quint32 typeValue = 0;
        readU32(fm, i + 12, &typeValue);
        if ((typeValue >> 16) != highWord)
            continue;
        const quint16 idx = static_cast<quint16>(typeValue & 0xFFFF);
        if (!out.contains(idx))
            out.insert(idx, ordinal);
        ++ordinal;
    }
    return out;
}

QByteArray fieldMapBytes(const PropsReader &props, quint32 key1, quint32 key2)
{
    QByteArray b = props.value(key1);
    return b.isEmpty() ? props.value(key2) : b;
}

} // namespace FieldMap
