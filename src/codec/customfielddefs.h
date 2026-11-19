// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef CUSTOMFIELDDEFS_H
#define CUSTOMFIELDDEFS_H

#include "scheduleio_export.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

// Microsoft Project task custom field definitions in an MPP14 file: the alias
// ("Risk" for Text1), the formula, the value list (lookup table) and the
// graphical indicators. Reverse-engineered 2026-11-17 from Project 2016 saves
// on dpr2hw3 (ScheduleVault/tools/customfield-samples-remote.vbs); field ids
// are full task field ids (0x0B40xxxx).
//
// "   114/TBkndTask/Props" item 0x04400001 is four sections:
//   A aliases  [u32 len][u32 len][u32 n][n x (u32 fieldId, u32 at-4)][n x 104-byte
//              UTF-16 slot]; len = section bytes - 4, each alias at most 50 chars
//   B, C       [u32 8][u32 8][u32 0] in every file seen (kept as found)
//   D          [u32 n][u32 n*88][n x 88-byte record] -- one per field with a
//              custom attribute (formula / value list) or graphical indicators:
//              [fieldId][type code][-1][slot family code][has value list]
//              [field GUID][value list GUID][28 zero bytes][0x80, or 0 for a
//              formula]; the field GUID is 000039B7-8BBE-4CEB-82C4-FA8C + field id.
// "   114/Props" item 0x02401390 holds one record per such field:
//   [u32 total][u32 n][n x (u32 fieldId, u32 offset)] then per field
//   [u32 attr][u32 -1][7 x u32 section end][formula][indicator rules]
//   - sections 1-4 end after the formula (UTF-16, NUL, zero padding: the
//     formula takes 2 * (chars + 1) bytes rounded up to 8; none = 36);
//     sections 5, 6, 7 end after the rules for non-summary rows, summary rows
//     and the project summary row.
//   - attr: 0x10 formula, 0x20 value list, 0x40 graphical indicators (with
//     0x02), 0x04 / 0x08 summary / project summary rows inherit the rules, 0x01
//     show data values in tool tips; a value list also sets 0x10000.
//   - a rule: [u32 image (PjIndicator)][u32 1000 + PjComparison][u32 1][u16 type]
//     [value]; "is any value" is just [image][0]. Types: 8 text (UTF-16 NUL),
//     5 number / cost (double; cost in hundredths), 0x13 date (MPP timestamp),
//     0x0B flag (u16), 3 duration (u32 tenths of a minute, u16 unit).
//   Field references in a formula are [MSPJ<field id>].
//   Item 0x02401392 (u16) is 1 once the file has any custom field definition.
// "   114/Props" item 0x0240138C lists the lookup tables: [u32 n][a 20-byte root
//   entry][per table: GUID, u32 1, u32 kind (3 text, 7 number, 5 cost, 4 date),
//   u32 0x2E, u32 0, u8 flags (0x05 restricted to the list, 0x04 not), u8 1,
//   u16 0, u16 PjListOrder]. A list's default value is its GUID at byte 52 of
//   the field's section-D record. "Append new values" is not kept in the file.
// Summary rows (attr): 0x40 | rollup << 12 for a rollup (PjSummaryCalc 0-7),
//   0x80 to use the formula; neither for none. Graphical indicators: 0x02.
//   A rule comparing with another field is [image][1000 + test][7][field id].
// "   114/TBkndOutlCode" (a stream quartet) holds the value lists: after the
//   template's rows, one row per value. Fixed: [u32 uid][u32 uid][u32 1][u16 0];
//   Fixed2: [value GUID][u16 list type][list GUID] (list type 21 text, 15
//   number, 9 cost, 4 date); var data (high word 0x1640): 0x16 the value (as
//   above), 0x08 its description, 0x17 its position in the list (double, 1-based).
namespace CustomFieldDefs {

inline constexpr quint32 kTaskDefinitionsKey = 0x04400001u;   // TBkndTask/Props
inline constexpr quint32 kFormulasKey = 0x02401390u;          // 114/Props
inline constexpr quint32 kDefinedKey = 0x02401392u;           // 114/Props, u16
inline constexpr quint32 kLookupDirectoryKey = 0x0240138Cu;   // 114/Props
inline constexpr int kMaxAliasLength = 50;

struct Alias {
    quint32 fieldId = 0;
    QString alias;
};

struct Indicator {
    quint32 image = 1;          // PjIndicator (1 = pjIndicatorSphereLime)
    int comparison = 11;        // PjComparison; 11 = is any value
    QByteArray value;           // [u16 type][value bytes], empty for "any value"
    quint32 field = 0;          // compare with this field instead (kind 7)
};

struct Formula {
    quint32 fieldId = 0;
    QString text;               // as stored: references are [MSPJ<id>]
    bool active = true;         // calculated (attr & 0x10)
    bool valueList = false;     // the field's attribute is its value list
    QList<Indicator> indicators;
    int summaryCalc = 10;       // PjSummaryCalc (10 none, 11 formula, 0-7 rollups)
};

// One section-D record.
struct AttributeField {
    quint32 fieldId = 0;
    bool formula = false;
    QByteArray valueListGuid;   // 16 bytes when the field has a value list
    QByteArray defaultGuid;     // the list value new tasks get, when there is one
};

struct LookupValue {
    QByteArray value;           // encoded as the field's kind (see above)
    QString description;
    QByteArray guid;            // 16 bytes; a new one is made when empty
};
struct LookupTable {
    QByteArray guid;            // 16 bytes
    quint16 type = 21;
    QList<LookupValue> values;
    bool restrict = true;       // only listed values may be entered
    int order = 0;              // PjListOrder
};

struct OutlCodeStreams {
    QByteArray fixedMeta, fixedData, fixed2Meta, fixed2Data, varMeta, var2Data;
};

QList<Alias> readAliases(const QByteArray &taskDefinitions);
QList<AttributeField> readAttributeFields(const QByteArray &taskDefinitions);
QList<Formula> readFormulas(const QByteArray &formulas);
QHash<QByteArray, LookupTable> readLookupTables(const OutlCodeStreams &streams);
// Restrict / order from item 0x0240138C onto tables read from TBkndOutlCode.
void readLookupDirectory(const QByteArray &directory, QHash<QByteArray, LookupTable> *tables);
QByteArray writeLookupDirectory(const QByteArray &existing, const QList<LookupTable> &tables);

// Rebuild item 0x04400001 from `existing` (the template's or the source
// file's), replacing section A with `aliases` and section D with `fields`;
// sections B and C are kept as found.
QByteArray writeTaskDefinitions(const QByteArray &existing, QList<Alias> aliases,
                                QList<AttributeField> fields);
QByteArray writeFormulas(QList<Formula> formulas);
// The template's TBkndOutlCode quartet with `tables` appended.
OutlCodeStreams writeLookupTables(const OutlCodeStreams &templateStreams,
                                  const QList<LookupTable> &tables);

// Typed values for a task custom field (by its kind): a rule's [type][value]
// and a value list's value, from and to the text the model keeps.
QByteArray encodeRuleValue(quint32 fieldId, const QString &text);
QString decodeRuleValue(quint32 fieldId, const QByteArray &typedValue);
QByteArray encodeListValue(quint32 fieldId, const QString &text);
QString decodeListValue(quint32 fieldId, const QByteArray &value);
// The value list type for a field kind, 0 for a kind Project gives no list.
quint16 listType(quint32 fieldId);
// A new GUID in Microsoft's byte order.
QByteArray newGuid();

// PjComparison <-> the model's rule names ("gt", "eq", "contains", ...); -1 /
// empty for none. PjIndicator <-> image names ("sphere-red", "flag-lime",
// "check-mark"; also ok / warning / critical); 0 / empty for none.
SCHEDULEIO_EXPORT int comparisonCode(const QString &name);
QString comparisonName(int code);
SCHEDULEIO_EXPORT quint32 imageCode(const QString &name);
QString imageName(quint32 code);
QStringList imageNames();

} // namespace CustomFieldDefs

#endif // CUSTOMFIELDDEFS_H
