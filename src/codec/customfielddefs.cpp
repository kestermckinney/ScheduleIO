// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "codec/customfielddefs.h"
#include "codec/fielddecoders.h"
#include "codec/mppfieldids.h"

#include <QDateTime>
#include <QStringList>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace CustomFieldDefs {
namespace {

constexpr int kAliasSlotBytes = 104;
constexpr int kAttributeRecordBytes = 88;
constexpr quint32 kFormulaBit = 0x10u;
constexpr quint32 kValueListBit = 0x20u;
constexpr quint32 kIndicatorBit = 0x02u;
constexpr quint32 kRollupBit = 0x40u;
constexpr quint32 kSummaryFormulaBit = 0x80u;
constexpr quint32 kInheritBits = 0x0Cu;       // summary + project summary rows inherit
constexpr quint32 kToolTipBit = 0x01u;
constexpr quint32 kValueListExtra = 0x10000u;
constexpr quint16 kVarHigh = 0x1640;
constexpr quint16 kVarValue = 0x16, kVarDescription = 0x08, kVarPosition = 0x17;
constexpr quint32 kMetaMagic = 0xFADFADBAu;
constexpr int kOutlMetaItem = 10;
constexpr int kTemplateRows = 4;

bool u32At(const QByteArray &b, int o, quint32 *v)
{
    if (o < 0 || o + 4 > b.size()) return false;
    *v = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + o));
    return true;
}

quint16 u16At(const QByteArray &b, int o)
{
    if (o < 0 || o + 2 > b.size()) return 0;
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(b.constData() + o));
}

double doubleAt(const QByteArray &b, int o)
{
    if (o < 0 || o + 8 > b.size()) return 0.0;
    double d = 0.0;
    memcpy(&d, b.constData() + o, 8);
    return d;
}

void put32(QByteArray &b, quint32 v)
{
    uchar raw[4];
    qToLittleEndian(v, raw);
    b.append(reinterpret_cast<const char *>(raw), 4);
}

void put16(QByteArray &b, quint16 v)
{
    uchar raw[2];
    qToLittleEndian(v, raw);
    b.append(reinterpret_cast<const char *>(raw), 2);
}

void putDouble(QByteArray &b, double v)
{
    char raw[8];
    memcpy(raw, &v, 8);
    b.append(raw, 8);
}

QString utf16z(const QByteArray &b, int o, int *end = nullptr)
{
    QString s;
    while (o + 1 < b.size()) {
        const char16_t c = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(b.constData() + o));
        o += 2;
        if (c == 0) break;
        s.append(QChar(c));
    }
    if (end) *end = o;
    return s;
}

QByteArray utf16Bytes(const QString &s, bool nul = false)
{
    QByteArray b;
    for (QChar c : s) put16(b, c.unicode());
    if (nul) put16(b, 0);
    return b;
}

QString slotOf(quint32 fieldId)
{
    for (const MppFieldIds::CustomFieldDef &d : MppFieldIds::taskCustomFields())
        if (d.index == quint16(fieldId & 0xFFFF)) return QString::fromLatin1(d.name);
    return QString();
}

enum class Kind { Text, Number, Cost, Date, Flag, Duration, Other };
Kind kindOf(quint32 fieldId)
{
    const QString s = slotOf(fieldId);
    if (s.startsWith(QLatin1String("Text"))) return Kind::Text;
    if (s.startsWith(QLatin1String("Number"))) return Kind::Number;
    if (s.startsWith(QLatin1String("Cost"))) return Kind::Cost;
    if (s.startsWith(QLatin1String("Date")) || s.startsWith(QLatin1String("Start"))
        || s.startsWith(QLatin1String("Finish"))) return Kind::Date;
    if (s.startsWith(QLatin1String("Flag"))) return Kind::Flag;
    if (s.startsWith(QLatin1String("Duration"))) return Kind::Duration;
    return Kind::Other;
}

// Project's codes for a field's section-D record: the data type and the slot
// family (from Project 2016's own saves, case cf-kinds).
void kindCodes(quint32 fieldId, quint32 *type, quint32 *family)
{
    const QString name = slotOf(fieldId);
    struct Code { const char *prefix; quint32 type; quint32 family; };
    static const Code codes[] = {
        { "Text", 8, 7 }, { "Start", 19, 6 }, { "Finish", 19, 3 }, { "Date", 19, 1 },
        { "Flag", 11, 4 }, { "Number", 5, 5 }, { "Duration", 3, 2 }, { "Cost", 5, 0 },
    };
    for (const Code &c : codes)
        if (name.startsWith(QLatin1String(c.prefix))) { *type = c.type; *family = c.family; return; }
    *type = 8;
    *family = 7;
}

int sectionEnd(const QByteArray &b, int o)
{
    quint32 len = 0;
    if (!u32At(b, o, &len) || o + 4 + int(len) > b.size()) return -1;
    return o + 4 + int(len);
}

QDateTime parseDateText(const QString &text)
{
    const QString t = text.trimmed();
    for (const char *f : { "yyyy-MM-ddTHH:mm:ss", "yyyy-MM-dd HH:mm", "yyyy-MM-dd", "M/d/yyyy h:mm AP",
                           "M/d/yyyy H:mm", "M/d/yyyy", "M/d/yy" }) {
        QDateTime d = QDateTime::fromString(t, QLatin1String(f));
        if (!d.isValid()) {
            const QDate dd = QDate::fromString(t, QLatin1String(f));
            if (dd.isValid()) d = QDateTime(dd, QTime(0, 0));
        }
        if (d.isValid()) {
            if (d.date().year() < 1950) d = d.addYears(100);
            return d;
        }
    }
    return QDateTime();
}

QString dateText(const QDateTime &d)
{
    if (!d.isValid()) return QStringLiteral("NA");
    return d.time() == QTime(0, 0) ? d.date().toString(Qt::ISODate)
                                   : d.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString numberText(double v)
{
    return QString::number(v, 'g', 15);
}

// One indicator rule at `o`; returns its end, or -1.
int readRule(const QByteArray &b, int o, int end, Indicator *rule)
{
    quint32 image = 0, test = 0;
    if (!u32At(b, o, &image) || !u32At(b, o + 4, &test) || o + 8 > end) return -1;
    rule->image = image;
    if (test == 0) {
        rule->comparison = 11;
        return o + 8;
    }
    rule->comparison = int(test) - 1000;
    quint32 kind = 0;
    u32At(b, o + 8, &kind);
    if (kind == 7) {
        if (!u32At(b, o + 12, &rule->field) || o + 16 > end) return -1;
        return o + 16;
    }
    int p = o + 12;
    const quint16 type = u16At(b, p);
    int size = 0;
    switch (type) {
    case 5: size = 8; break;
    case 0x13: size = 4; break;
    case 0x0B: size = 2; break;
    case 3: size = 6; break;
    case 8: {
        int e = 0;
        utf16z(b, p + 2, &e);
        size = e - (p + 2);
        break;
    }
    default: return -1;
    }
    if (p + 2 + size > end) return -1;
    rule->value = b.mid(p, 2 + size);
    return p + 2 + size;
}

QByteArray ruleBytes(const Indicator &r)
{
    QByteArray b;
    put32(b, r.image);
    if (r.comparison == 11 || (r.value.isEmpty() && r.field == 0)) {
        put32(b, 0);
        return b;
    }
    put32(b, quint32(1000 + r.comparison));
    if (r.field) {
        put32(b, 7);
        put32(b, r.field);
        return b;
    }
    put32(b, 1);
    b += r.value;
    return b;
}

QByteArray fixedMeta(int items, int dataSize, const QByteArray &body)
{
    QByteArray b;
    put32(b, kMetaMagic);
    put32(b, 4);
    put32(b, quint32(items));
    put32(b, quint32(dataSize));
    return b + body;
}

} // namespace

QList<Alias> readAliases(const QByteArray &b)
{
    QList<Alias> out;
    quint32 len = 0, n = 0;
    if (!u32At(b, 0, &len) || !u32At(b, 8, &n)) return out;
    const int end = qMin(int(b.size()), 4 + int(len));
    for (quint32 i = 0; i < n; ++i) {
        quint32 fieldId = 0, at = 0;
        if (!u32At(b, 12 + int(i) * 8, &fieldId) || !u32At(b, 16 + int(i) * 8, &at)) break;
        const int o = int(at) + 4;
        if (o <= 0 || o >= end) continue;
        const QString alias = utf16z(b.left(end), o);
        if (!alias.isEmpty()) out.append({ fieldId, alias });
    }
    return out;
}

QList<AttributeField> readAttributeFields(const QByteArray &b)
{
    QList<AttributeField> out;
    int o = 0;
    for (int i = 0; i < 3 && o >= 0; ++i) o = sectionEnd(b, o);
    quint32 n = 0;
    if (o < 0 || !u32At(b, o, &n)) return out;
    for (quint32 i = 0; i < n; ++i) {
        const int r = o + 8 + int(i) * kAttributeRecordBytes;
        if (r + kAttributeRecordBytes > b.size()) break;
        AttributeField f;
        quint32 hasList = 0, tail = 0;
        u32At(b, r, &f.fieldId);
        u32At(b, r + 16, &hasList);
        u32At(b, r + 84, &tail);
        f.formula = tail == 0;
        if (hasList) f.valueListGuid = b.mid(r + 36, 16);
        const QByteArray def = b.mid(r + 52, 16);
        if (def.count('\0') != 16) f.defaultGuid = def;
        out.append(f);
    }
    return out;
}

QList<Formula> readFormulas(const QByteArray &b)
{
    QList<Formula> out;
    quint32 n = 0;
    if (!u32At(b, 4, &n)) return out;
    for (quint32 i = 0; i < n; ++i) {
        quint32 fieldId = 0, at = 0, attr = 0;
        if (!u32At(b, 8 + int(i) * 8, &fieldId) || !u32At(b, 12 + int(i) * 8, &at)) break;
        if (!u32At(b, int(at), &attr)) continue;
        quint32 ends[7] = {};
        for (int s = 0; s < 7; ++s) u32At(b, int(at) + 8 + 4 * s, &ends[s]);
        Formula f;
        f.fieldId = fieldId;
        if (ends[0] > 36) f.text = utf16z(b.left(int(at) + int(ends[0])), int(at) + 36);
        f.active = (attr & kFormulaBit) != 0 && !f.text.isEmpty();
        f.valueList = (attr & kValueListBit) != 0;
        f.summaryCalc = (attr & kSummaryFormulaBit) ? 11 : (attr & kRollupBit) ? int((attr >> 12) & 0xF) : 10;
        if (attr & kIndicatorBit) {
            const int end = int(at) + int(ends[4]);
            for (int o = int(at) + int(ends[3]); o < end; ) {
                Indicator r;
                const int next = readRule(b, o, end, &r);
                if (next < 0) break;
                f.indicators.append(r);
                o = next;
            }
        }
        if (!f.text.isEmpty() || f.valueList || !f.indicators.isEmpty() || f.summaryCalc != 10)
            out.append(f);
    }
    return out;
}

QHash<QByteArray, LookupTable> readLookupTables(const OutlCodeStreams &s)
{
    QHash<QByteArray, LookupTable> tables;
    QHash<quint32, QByteArray> tableOf;     // value uid -> list GUID
    QHash<quint32, quint16> typeOf;
    QHash<quint32, QByteArray> valueGuid;
    quint32 n2 = 0;
    u32At(s.fixed2Meta, 8, &n2);
    // Fixed2 rows pair with Fixed rows; a value row's Fixed record starts with its uid.
    quint32 n1 = 0;
    u32At(s.fixedMeta, 8, &n1);
    for (quint32 i = 0; i < qMin(n1, n2); ++i) {
        quint32 off1 = 0, off2 = 0;
        if (!u32At(s.fixedMeta, 16 + int(i) * kOutlMetaItem + 4, &off1)
            || !u32At(s.fixed2Meta, 16 + int(i) * kOutlMetaItem + 4, &off2)) break;
        quint32 uid = 0, level = 0;
        u32At(s.fixedData, int(off1), &uid);
        u32At(s.fixedData, int(off1) + 8, &level);
        if (level != 1 || int(off2) + 34 > s.fixed2Data.size()) continue;
        const quint16 type = u16At(s.fixed2Data, int(off2) + 16);
        const QByteArray guid = s.fixed2Data.mid(int(off2) + 18, 16);
        if (guid.count('\0') == 16) continue;
        tableOf.insert(uid, guid);
        typeOf.insert(uid, type);
        valueGuid.insert(uid, s.fixed2Data.mid(int(off2), 16));
    }
    struct Raw { double position = 0; LookupValue v; };
    QHash<quint32, Raw> raw;
    quint32 nv = 0;
    u32At(s.varMeta, 8, &nv);
    for (quint32 i = 0; i < nv; ++i) {
        const int m = 24 + int(i) * 12;
        quint32 uid = 0, off = 0, len = 0;
        if (!u32At(s.varMeta, m, &uid) || !u32At(s.varMeta, m + 4, &off)) break;
        const quint16 key = u16At(s.varMeta, m + 8);
        if (!tableOf.contains(uid) || !u32At(s.var2Data, int(off), &len)) continue;
        const QByteArray data = s.var2Data.mid(int(off) + 4, int(len));
        Raw &r = raw[uid];
        if (key == kVarValue) r.v.value = data;
        else if (key == kVarDescription) r.v.description = utf16z(data, 0);
        else if (key == kVarPosition) r.position = doubleAt(data, 0);
    }
    QList<quint32> uids = raw.keys();
    std::sort(uids.begin(), uids.end(), [&](quint32 a, quint32 b) {
        return raw[a].position != raw[b].position ? raw[a].position < raw[b].position : a < b;
    });
    for (quint32 uid : uids) {
        LookupTable &t = tables[tableOf.value(uid)];
        t.guid = tableOf.value(uid);
        t.type = typeOf.value(uid);
        LookupValue v = raw[uid].v;
        v.guid = valueGuid.value(uid);
        t.values.append(v);
    }
    return tables;
}

QByteArray writeTaskDefinitions(const QByteArray &existing, QList<Alias> aliases,
                                QList<AttributeField> fields)
{
    std::sort(aliases.begin(), aliases.end(),
              [](const Alias &a, const Alias &b) { return a.fieldId < b.fieldId; });
    std::sort(fields.begin(), fields.end(),
              [](const AttributeField &a, const AttributeField &b) { return a.fieldId < b.fieldId; });

    // Section A.
    QByteArray a;
    const int n = int(aliases.size());
    const int stringsAt = 12 + 8 * n;
    put32(a, quint32(stringsAt - 4 + kAliasSlotBytes * n));
    put32(a, quint32(stringsAt - 4 + kAliasSlotBytes * n));
    put32(a, quint32(n));
    for (int i = 0; i < n; ++i) {
        put32(a, aliases.at(i).fieldId);
        put32(a, quint32(stringsAt + kAliasSlotBytes * i - 4));
    }
    for (const Alias &al : std::as_const(aliases)) {
        QByteArray slot = utf16Bytes(al.alias.left(kMaxAliasLength));
        slot.append(kAliasSlotBytes - slot.size(), '\0');
        a += slot;
    }

    // Sections B and C as found (the empty form when there is nothing to keep).
    QByteArray bc;
    const int aEnd = sectionEnd(existing, 0);
    const int bEnd = aEnd < 0 ? -1 : sectionEnd(existing, aEnd);
    const int cEnd = bEnd < 0 ? -1 : sectionEnd(existing, bEnd);
    if (cEnd > 0) {
        bc = existing.mid(aEnd, cEnd - aEnd);
    } else {
        for (int i = 0; i < 2; ++i) { put32(bc, 8); put32(bc, 8); put32(bc, 0); }
    }

    // Section D.
    QByteArray d;
    put32(d, quint32(fields.size()));
    put32(d, quint32(fields.size() * kAttributeRecordBytes));
    for (const AttributeField &f : std::as_const(fields)) {
        QByteArray r;
        quint32 type = 0, family = 0;
        kindCodes(f.fieldId, &type, &family);
        put32(r, f.fieldId);
        put32(r, type);
        put32(r, 0xFFFFFFFFu);
        put32(r, family);
        put32(r, f.valueListGuid.size() == 16 ? 1 : 0);
        static const uchar guidHead[12] = { 0xb7, 0x39, 0x00, 0x00, 0xbe, 0x8b, 0xeb, 0x4c,
                                            0x82, 0xc4, 0xfa, 0x8c };
        r.append(reinterpret_cast<const char *>(guidHead), 12);
        uchar id[4];
        qToBigEndian(f.fieldId, id);
        r.append(reinterpret_cast<const char *>(id), 4);
        r += f.valueListGuid.size() == 16 ? f.valueListGuid : QByteArray(16, '\0');
        r += f.defaultGuid.size() == 16 ? f.defaultGuid : QByteArray(16, '\0');
        r.append(kAttributeRecordBytes - 4 - r.size(), '\0');
        put32(r, f.formula ? 0 : 0x80);
        d += r;
    }
    return a + bc + d;
}

QByteArray writeFormulas(QList<Formula> formulas)
{
    std::sort(formulas.begin(), formulas.end(),
              [](const Formula &a, const Formula &b) { return a.fieldId < b.fieldId; });
    QList<QByteArray> records;
    for (const Formula &f : std::as_const(formulas)) {
        const bool formula = f.active && !f.text.isEmpty();
        quint32 attr = 0;
        if (formula) attr |= kFormulaBit;
        else if (f.valueList) attr |= kValueListBit | kValueListExtra;
        if (f.indicators.isEmpty()) attr |= kInheritBits | kToolTipBit;
        else attr |= kIndicatorBit | kInheritBits;
        if (f.summaryCalc == 11) attr |= kSummaryFormulaBit;
        else if (f.summaryCalc >= 0 && f.summaryCalc <= 7) attr |= kRollupBit | (quint32(f.summaryCalc) << 12);
        // A formula Project keeps but does not calculate.
        if (!f.active && !f.text.isEmpty()) attr &= ~kFormulaBit;

        const QByteArray text = utf16Bytes(f.text);
        const int textBytes = f.text.isEmpty() ? 0 : (int(text.size()) + 2 + 7) / 8 * 8;
        QByteArray rules;
        for (const Indicator &r : f.indicators) rules += ruleBytes(r);
        const quint32 formulaEnd = quint32(36 + textBytes);
        // Summary and project summary rows inherit the rules: Project writes
        // the list three times.
        const quint32 e1 = formulaEnd + quint32(rules.size());
        const quint32 e2 = e1 + quint32(rules.size());
        const quint32 e3 = e2 + quint32(rules.size());
        QByteArray r;
        put32(r, attr);
        put32(r, 0xFFFFFFFFu);
        for (int i = 0; i < 4; ++i) put32(r, formulaEnd);
        put32(r, e1);
        put32(r, e2);
        put32(r, e3);
        r += text;
        r.append(int(formulaEnd) - r.size(), '\0');
        r += rules + rules + rules;
        records.append(r);
    }
    QByteArray out;
    const int n = int(records.size());
    int at = 8 + 8 * n;
    QByteArray index;
    for (int i = 0; i < n; ++i) {
        put32(index, formulas.at(i).fieldId);
        put32(index, quint32(at));
        at += int(records.at(i).size());
    }
    put32(out, quint32(at));
    put32(out, quint32(n));
    out += index;
    for (const QByteArray &r : std::as_const(records)) out += r;
    return out;
}

OutlCodeStreams writeLookupTables(const OutlCodeStreams &t, const QList<LookupTable> &tables)
{
    // The template's own rows (placeholders and its root row), verbatim.
    quint32 n1 = 0, n2 = 0, nv = 0;
    u32At(t.fixedMeta, 8, &n1);
    u32At(t.fixed2Meta, 8, &n2);
    u32At(t.varMeta, 8, &nv);
    const int keep1 = qMin<int>(int(n1), kTemplateRows), keep2 = qMin<int>(int(n2), kTemplateRows);
    QByteArray m1 = t.fixedMeta.mid(16, keep1 * kOutlMetaItem), d1 = t.fixedData;
    QByteArray m2 = t.fixed2Meta.mid(16, keep2 * kOutlMetaItem), d2 = t.fixed2Data;
    struct Var { quint32 uid; quint16 key; QByteArray data; };
    QList<Var> vars;
    for (quint32 i = 0; i < nv; ++i) {
        const int m = 24 + int(i) * 12;
        quint32 uid = 0, off = 0, len = 0;
        u32At(t.varMeta, m, &uid);
        u32At(t.varMeta, m + 4, &off);
        u32At(t.var2Data, int(off), &len);
        vars.append({ uid, u16At(t.varMeta, m + 8), t.var2Data.mid(int(off) + 4, int(len)) });
    }
    int items1 = keep1, items2 = keep2;
    quint32 uid = 0;
    for (const Var &v : std::as_const(vars)) uid = qMax(uid, v.uid);
    for (const LookupTable &table : tables) {
        int position = 0;
        for (const LookupValue &value : table.values) {
            ++uid;
            ++position;
            const bool described = !value.description.isEmpty();
            QByteArray item;
            put32(item, quint32(described ? 3 : 2) << 16);
            put32(item, quint32(d1.size()));
            put16(item, described ? 0x03D3 : 0x01D3);
            m1 += item;
            put32(d1, uid);
            put32(d1, uid);
            put32(d1, 1);
            put16(d1, 0);
            ++items1;

            QByteArray item2;
            put32(item2, 0);
            put32(item2, quint32(d2.size()));
            put16(item2, 0x001F);
            m2 += item2;
            d2 += value.guid.size() == 16 ? value.guid : newGuid();
            put16(d2, table.type);
            d2 += table.guid;
            ++items2;

            if (described) vars.append({ uid, kVarDescription, utf16Bytes(value.description, true) });
            vars.append({ uid, kVarValue, value.value });
            QByteArray pos;
            putDouble(pos, double(position));
            vars.append({ uid, kVarPosition, pos });
        }
    }
    OutlCodeStreams out;
    out.fixedMeta = fixedMeta(items1, int(d1.size()), m1);
    out.fixedData = d1;
    out.fixed2Meta = fixedMeta(items2, int(d2.size()), m2);
    out.fixed2Data = d2;
    QByteArray pool, meta;
    for (const Var &v : std::as_const(vars)) {
        put32(meta, v.uid);
        put32(meta, quint32(pool.size()));
        put16(meta, v.key);
        put16(meta, kVarHigh);
        put32(pool, quint32(v.data.size()));
        pool += v.data;
    }
    QByteArray head;
    put32(head, kMetaMagic);
    put32(head, 0);
    put32(head, quint32(vars.size()));
    put32(head, 0);
    put32(head, 0);
    put32(head, quint32(pool.size()));
    out.varMeta = head + meta;
    out.var2Data = pool;
    return out;
}

namespace {
constexpr int kDirectoryRoot = 20, kDirectoryEntry = 38;
quint32 directoryKind(quint16 listType)
{
    switch (listType) {
    case 15: return 7;   // number
    case 9: return 5;    // cost
    case 4: return 4;    // date
    default: return 3;   // text
    }
}
} // namespace

void readLookupDirectory(const QByteArray &d, QHash<QByteArray, LookupTable> *tables)
{
    quint32 n = 0;
    if (!u32At(d, 0, &n) || n == 0) return;
    for (int o = 4 + kDirectoryRoot; o + kDirectoryEntry <= d.size(); o += kDirectoryEntry) {
        auto it = tables->find(d.mid(o, 16));
        if (it == tables->end()) continue;
        it->restrict = (quint8(d.at(o + 32)) & 0x01) != 0;
        it->order = u16At(d, o + 36);
    }
}

QByteArray writeLookupDirectory(const QByteArray &existing, const QList<LookupTable> &tables)
{
    QByteArray root = existing.size() >= 4 + kDirectoryRoot ? existing.mid(4, kDirectoryRoot)
                                                            : QByteArray(kDirectoryRoot, '\0');
    QByteArray out;
    put32(out, quint32(1 + tables.size()));
    out += root;
    for (const LookupTable &t : tables) {
        out += t.guid;
        put32(out, 1);
        put32(out, directoryKind(t.type));
        put32(out, 0x2E);
        put32(out, 0);
        out.append(char(t.restrict ? 0x05 : 0x04));
        out.append(char(0x01));
        put16(out, 0);
        put16(out, quint16(t.order));
    }
    return out;
}

QByteArray encodeRuleValue(quint32 fieldId, const QString &text)
{
    QByteArray b;
    bool ok = false;
    switch (kindOf(fieldId)) {
    case Kind::Number:
    case Kind::Cost: {
        double v = text.trimmed().toDouble(&ok);
        if (!ok) return {};
        if (kindOf(fieldId) == Kind::Cost) v *= 100.0;
        put16(b, 5);
        putDouble(b, v);
        return b;
    }
    case Kind::Date: {
        const QDateTime d = parseDateText(text);
        if (!d.isValid()) return {};
        put16(b, 0x13);
        put32(b, FieldDecoders::encodeMppTimestamp(d));
        return b;
    }
    case Kind::Flag: {
        const QString t = text.trimmed().toLower();
        put16(b, 0x0B);
        put16(b, (t == QLatin1String("yes") || t == QLatin1String("true") || t == QLatin1String("1")) ? 1 : 0);
        return b;
    }
    case Kind::Duration: {
        const double minutes = text.trimmed().toDouble(&ok);
        if (!ok) return {};
        put16(b, 3);
        put32(b, quint32(qint32(std::llround(minutes * 10.0))));
        put16(b, 7);   // Project writes the unit as days
        return b;
    }
    default:
        put16(b, 8);
        b += utf16Bytes(text, true);
        return b;
    }
}

QString decodeRuleValue(quint32 fieldId, const QByteArray &v)
{
    const quint16 type = u16At(v, 0);
    switch (type) {
    case 5: return numberText(doubleAt(v, 2) / (kindOf(fieldId) == Kind::Cost ? 100.0 : 1.0));
    case 0x13: return dateText(FieldDecoders::decodeMppTimestamp(v, 2));
    case 0x0B: return u16At(v, 2) ? QStringLiteral("Yes") : QStringLiteral("No");
    case 3: {
        quint32 raw = 0;
        u32At(v, 2, &raw);
        return numberText(double(qint32(raw)) / 10.0);
    }
    case 8: return utf16z(v, 2);
    default: return QString();
    }
}

QByteArray encodeListValue(quint32 fieldId, const QString &text)
{
    QByteArray b;
    bool ok = false;
    switch (kindOf(fieldId)) {
    case Kind::Number:
    case Kind::Cost: {
        double v = text.trimmed().toDouble(&ok);
        if (!ok) return {};
        putDouble(b, kindOf(fieldId) == Kind::Cost ? v * 100.0 : v);
        return b;
    }
    case Kind::Date: {
        const QDateTime d = parseDateText(text);
        if (!d.isValid()) return {};
        put32(b, FieldDecoders::encodeMppTimestamp(d));
        return b;
    }
    case Kind::Text:
        return utf16Bytes(text, true);
    default:
        return {};
    }
}

QString decodeListValue(quint32 fieldId, const QByteArray &v)
{
    switch (kindOf(fieldId)) {
    case Kind::Number: return numberText(doubleAt(v, 0));
    case Kind::Cost: return numberText(doubleAt(v, 0) / 100.0);
    case Kind::Date: return dateText(FieldDecoders::decodeMppTimestamp(v, 0));
    default: return utf16z(v, 0);
    }
}

quint16 listType(quint32 fieldId)
{
    switch (kindOf(fieldId)) {
    case Kind::Text: return 21;
    case Kind::Number: return 15;
    case Kind::Cost: return 9;
    case Kind::Date: return 4;
    default: return 0;
    }
}

QByteArray newGuid()
{
    // QUuid's RFC 4122 bytes are big-endian; Microsoft stores the first three
    // groups little-endian.
    const QByteArray be = QUuid::createUuid().toRfc4122();
    QByteArray le = be;
    std::reverse(le.begin(), le.begin() + 4);
    std::reverse(le.begin() + 4, le.begin() + 6);
    std::reverse(le.begin() + 6, le.begin() + 8);
    return le;
}

namespace {
struct NamedCode { const char *name; int code; };
const NamedCode kComparisons[] = {
    { "gt", 1 }, { "lt", 2 }, { "ge", 3 }, { "le", 4 }, { "eq", 5 }, { "ne", 6 },
    { "contains", 7 }, { "notcontains", 9 }, { "containsexactly", 10 }, { "any", 11 },
};
// PjIndicator, in order from 1.
const char *const kImages[] = {
    "sphere-lime", "sphere-yellow", "sphere-red", "sphere-black", "sphere-white",
    "sphere-aqua", "sphere-green", "sphere-blue", "sphere-fuschia", "sphere-purple",
    "sphere-maroon", "sphere-gray", "sphere-silver", "flag-lime", "flag-yellow",
    "flag-red", "flag-white", "flag-aqua", "flag-blue", "flag-fuschia", "flag-silver",
    "square-lime", "square-yellow", "square-red", "square-black", "square-white",
    "plus-lime", "plus-yellow", "plus-red", "plus-black", "plus-white",
    "minus-lime", "minus-yellow", "minus-red", "minus-black", "minus-white",
    "diamond-lime", "diamond-yellow", "diamond-red", "arrow-left", "arrow-right",
    "arrow-double", "arrow-up", "arrow-down", "circle-solid-fill", "circle-bottom-fill",
    "circle-left-fill", "circle-top-fill", "circle-right-fill", "circle-round-fill",
    "circle-hollow", "light-bulb-off", "light-bulb-on", "check-mark", "delete-mark",
    "question-mark", "clock", "push-pin", "face-happy-yellow", "face-happy-lime",
    "face-straight-yellow", "face-straight-aqua", "face-sad-yellow", "face-sad-red",
    "dash-gray",
};
} // namespace

int comparisonCode(const QString &name)
{
    const QString n = name.trimmed().toLower();
    for (const NamedCode &c : kComparisons)
        if (n == QLatin1String(c.name)) return c.code;
    return -1;
}

QString comparisonName(int code)
{
    for (const NamedCode &c : kComparisons)
        if (c.code == code) return QLatin1String(c.name);
    return QString();
}

quint32 imageCode(const QString &name)
{
    QString n = name.trimmed().toLower();
    n.replace(QLatin1Char('_'), QLatin1Char('-')).replace(QLatin1Char(' '), QLatin1Char('-'));
    if (n == QLatin1String("ok") || n == QLatin1String("good")) return 1;
    if (n == QLatin1String("warning") || n == QLatin1String("caution")) return 2;
    if (n == QLatin1String("critical") || n == QLatin1String("bad") || n == QLatin1String("error")
        || n == QLatin1String("late")) return 3;
    for (int i = 0; i < int(std::size(kImages)); ++i)
        if (n == QLatin1String(kImages[i])) return quint32(i + 1);
    return 0;
}

QString imageName(quint32 code)
{
    return code >= 1 && code <= quint32(std::size(kImages)) ? QLatin1String(kImages[code - 1]) : QString();
}

QStringList imageNames()
{
    QStringList out;
    for (const char *n : kImages) out.append(QLatin1String(n));
    return out;
}

} // namespace CustomFieldDefs
