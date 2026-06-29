// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPFIELDIDS_H
#define MPPFIELDIDS_H

#include <QVector>
#include <QtGlobal>

// Field-id tables for cost, baseline and custom ("extended") fields, ported from
// MPXJ's MPPTaskField / MPPResourceField / MPPAssignmentField FIELD_ARRAY
// (github.com/joniles/mpxj). A field's low-word index equals its var-data key and
// its FixedData field-map index; the high word selects the entity (task 0x0B40,
// resource 0x0C40, assignment 0x0F40). Whether a given field lands in FixedData or
// Var2Data varies per file, so the reader resolves the location from the field map
// and only uses these tables to know which indices to look for and how to decode.
namespace MppFieldIds {

// Sentinel for "this entity has no such field".
constexpr quint16 kAbsent = 0xFFFF;

enum class FieldKind {
    String,     // length-prefixed UTF-16 (Text*, Outline Code*)
    Number,     // 8-byte double (Number*)
    Currency,   // 8-byte double in the project currency unit (Cost*)
    DateTime,   // 4-byte MPP timestamp (Date*, Start*, Finish*)
    Duration,   // u32 tenths-of-a-minute (Duration*)
    Bool,       // single byte / u16 flag (Flag*)
};

// A custom-field slot: its index, human name and how to decode its value.
struct CustomFieldDef {
    quint16 index;
    const char *name;
    FieldKind kind;
};

// The five cost scalars of an entity (kAbsent where not applicable).
struct CostFields {
    quint16 cost;
    quint16 fixedCost;
    quint16 actualCost;
    quint16 remainingCost;
    quint16 costVariance;
};

// One baseline set's field indices (kAbsent where the entity lacks that field).
struct BaselineSet {
    quint16 cost;
    quint16 work;
    quint16 start;
    quint16 finish;
    quint16 duration;
};

// High words identifying each entity's fields within a field map / var data.
constexpr quint16 kTaskHigh = 0x0B40;
constexpr quint16 kResourceHigh = 0x0C40;
constexpr quint16 kAssignmentHigh = 0x0F40;

// Baseline sets are indexed 0..10 (0 = current baseline).
constexpr int kBaselineCount = 11;

extern const CostFields taskCost;
extern const BaselineSet taskBaselines[kBaselineCount];
const QVector<CustomFieldDef> &taskCustomFields();

extern const CostFields resourceCost;
extern const BaselineSet resourceBaselines[kBaselineCount];
const QVector<CustomFieldDef> &resourceCustomFields();

extern const CostFields assignmentCost;
extern const BaselineSet assignmentBaselines[kBaselineCount];
const QVector<CustomFieldDef> &assignmentCustomFields();

} // namespace MppFieldIds

#endif // MPPFIELDIDS_H
