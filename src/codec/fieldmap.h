// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef FIELDMAP_H
#define FIELDMAP_H

#include <QByteArray>
#include <QHash>
#include <QtGlobal>

class PropsReader;

// Parsers for the per-entity field maps stored in "   114/Props" (28-byte
// entries; see DECODING_NOTES.md). Shared by the reader (docserializer.cpp)
// and the MPP14 writer, so both resolve a field index to the same location.
namespace FieldMap {

// Where a field's value lives for an entity. A field can be in FixedData/Fixed2Data
// (block 0/1 at a byte offset) or in Var2Data (var == true, keyed by the field
// index). Fixed is preferred when both are present.
struct EntityFieldLoc {
    int block = -1;     // fixed block index (0 = FixedData, 1 = Fixed2Data), else -1
    int offset = -1;    // byte offset within that fixed block
    bool var = false;   // also/only present as variable-length data
};

// Parse a field map (28-byte entries) into per-index locations for one entity
// (high word: task 0x0B40, resource 0x0C40, assignment 0x0F40). The fixed-block
// index is tracked across ALL fixed entries (it increments when an offset steps
// backwards), so block 0/1 stay aligned with FixedData/Fixed2Data. Keeps the
// first fixed offset seen per index.
QHash<quint16, EntityFieldLoc> entityFieldLocations(const QByteArray &fm, quint16 highWord);

// Generic: map field index -> block-0 fixed-data offset for an entity field map,
// keeping only entries whose high word matches the entity and that live in block 0.
QHash<quint16, int> block0FixedOffsets(const QByteArray &fm, quint16 highWord);

// Fetch an entity field map from the project Props (primary key, else fallback).
QByteArray fieldMapBytes(const PropsReader &props, quint32 key1, quint32 key2);

} // namespace FieldMap

#endif // FIELDMAP_H
