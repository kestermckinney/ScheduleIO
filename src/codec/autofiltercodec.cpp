// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/autofiltercodec.h"
#include "codec/mppfieldids.h"

#include <QtEndian>
#include <QVector>

#include <cstring>

using schedule::AutoFilterCondition;
using schedule::AutoFilterValue;
using schedule::ColumnAutoFilter;

namespace {

constexpr int kBlockSize = 36;
constexpr int kBlocksStart = 0x14;     // first block, relative to B
constexpr int kEntryHeader = 12;       // field, length, PjAutoFilterType
constexpr quint16 kNone = 0xFFFF;
constexpr quint16 kAnd = 0x19, kOr = 0x1A;
constexpr quint32 kOperandField = 7, kOperandValue = 1;
constexpr quint16 kTypeText = 8, kTypeDateOrDuration = 0x13, kTypePercent = 2,
                  kTypeDouble = 5, kTypeFlag = 0x0B, kTypeList = 0x4000;
constexpr quint16 kWorkUnits = 2;      // what Project writes with a work value
constexpr quint32 kNaDate = 0xFFFFFFFFu; // "NA" -- no date

quint16 rd16(const QByteArray &b, int o)
{
    return o >= 0 && o + 2 <= b.size()
        ? qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(b.constData() + o)) : 0;
}
quint32 rd32(const QByteArray &b, int o)
{
    return o >= 0 && o + 4 <= b.size()
        ? qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + o)) : 0;
}
double rdDouble(const QByteArray &b, int o)
{
    if (o < 0 || o + 8 > b.size())
        return 0.0;
    const quint64 bits = qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(b.constData() + o));
    double v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}
void wr16(QByteArray &b, int o, quint16 v) { qToLittleEndian(v, reinterpret_cast<uchar *>(b.data() + o)); }
void wr32(QByteArray &b, int o, quint32 v) { qToLittleEndian(v, reinterpret_cast<uchar *>(b.data() + o)); }
void wrDouble(QByteArray &b, int o, double v)
{
    quint64 bits;
    std::memcpy(&bits, &v, sizeof v);
    qToLittleEndian(bits, reinterpret_cast<uchar *>(b.data() + o));
}
void app16(QByteArray &b, quint16 v) { const int o = b.size(); b.resize(o + 2); wr16(b, o, v); }
void app32(QByteArray &b, quint32 v) { const int o = b.size(); b.resize(o + 4); wr32(b, o, v); }
void appString(QByteArray &b, const QString &s)
{
    for (const QChar c : s)
        app16(b, c.unicode());
    app16(b, 0);
}
// A UTF-16 z-string at `o`; *end receives the offset after its terminator.
QString rdString(const QByteArray &b, int o, int *end)
{
    QString s;
    while (o + 2 <= b.size()) {
        const quint16 c = rd16(b, o);
        o += 2;
        if (c == 0)
            break;
        s.append(QChar(c));
    }
    if (end)
        *end = o;
    return s;
}

const QDate kEpoch(1984, 1, 1);

// ---- decoding --------------------------------------------------------------

struct Parsed {
    QVector<QByteArray> blocks;   // in file order
    QStringList texts;            // the text values, in block order
};

int blockIndex(quint16 offset, int count)
{
    if (offset == kNone || offset < kBlocksStart || (offset - kBlocksStart) % kBlockSize)
        return -1;
    const int i = (offset - kBlocksStart) / kBlockSize;
    return i < count ? i : -1;
}

// A typed value: `type` and the 8 payload bytes at `at` of `b` (a value
// block's u16@12 / @14, or a list item's u16@0 / @2); `units` the value
// block's u16@24.
AutoFilterValue decodePayload(const QByteArray &b, int at, quint16 type, quint16 units,
                              quint32 fieldId, const QString &text)
{
    AutoFilterValue v;
    switch (type) {
    case kTypeText:
        v.type = AutoFilterValue::Type::Text;
        v.text = text;
        break;
    case kTypeDateOrDuration: {
        const quint32 raw = rd32(b, at);
        if (units != 0) {
            v.type = AutoFilterValue::Type::Duration;
            v.millis = qint64(raw) * 6000;
            v.durationUnit = units;
        } else {
            v.type = AutoFilterValue::Type::Date;
            v.date = AutoFilterCodec::fromMppDate(raw);
            v.anyTime = raw != kNaDate && (raw & 0xFFFF) == 0xFFFF;
        }
        break;
    }
    case kTypePercent:
        v.type = AutoFilterValue::Type::Percent;
        v.number = rd16(b, at);
        break;
    case kTypeDouble:
        if (units == kWorkUnits) {
            v.type = AutoFilterValue::Type::Work;
            v.millis = qRound64(rdDouble(b, at) * 60.0);
        } else if (AutoFilterCodec::isCurrencyField(fieldId)) {
            v.type = AutoFilterValue::Type::Cost;
            v.number = rdDouble(b, at) / 100.0;   // stored in hundredths
        } else {
            v.type = AutoFilterValue::Type::Number;
            v.number = rdDouble(b, at);
        }
        break;
    case kTypeFlag:
        v.type = AutoFilterValue::Type::Flag;
        v.number = rd16(b, at) ? 1 : 0;
        break;
    default:
        v.type = AutoFilterValue::Type::Text;
        v.text = text;
        break;
    }
    return v;
}

// The type code and 8 payload bytes of a value (shared by value blocks and
// list items); *units receives the units a value block records.
quint16 encodePayload(const AutoFilterValue &v, QByteArray &b, int at, quint16 *units)
{
    *units = 0;
    switch (v.type) {
    case AutoFilterValue::Type::Text:
        return kTypeText;
    case AutoFilterValue::Type::Date: {
        quint32 raw = AutoFilterCodec::toMppDate(v.date);
        if (v.anyTime && raw != kNaDate)
            raw |= 0xFFFF;
        wr32(b, at, raw);
        return kTypeDateOrDuration;
    }
    case AutoFilterValue::Type::Duration:
        wr32(b, at, quint32(qMax<qint64>(0, v.millis / 6000)));
        *units = quint16(v.durationUnit > 0 ? v.durationUnit : 7);
        return kTypeDateOrDuration;
    case AutoFilterValue::Type::Work:
        wrDouble(b, at, double(v.millis) / 60.0);
        *units = kWorkUnits;
        return kTypeDouble;
    case AutoFilterValue::Type::Percent:
        wr16(b, at, quint16(qBound(0, qRound(v.number), 65535)));
        return kTypePercent;
    case AutoFilterValue::Type::Flag:
        wr16(b, at, v.number != 0.0 ? 1 : 0);
        return kTypeFlag;
    case AutoFilterValue::Type::Cost:
        wrDouble(b, at, v.number * 100.0);   // hundredths of the currency unit
        return kTypeDouble;
    case AutoFilterValue::Type::Number:
        wrDouble(b, at, v.number);
        return kTypeDouble;
    }
    return kTypeText;
}

// A test block (and its field + value children) as a condition.
bool decodeCondition(const Parsed &p, int index, const QHash<int, QString> &textOf,
                     AutoFilterCondition *out, quint32 *fieldId)
{
    const QByteArray &op = p.blocks[index];
    out->test = rd16(op, 0);
    out->values.clear();
    for (int c = blockIndex(rd16(op, 32), p.blocks.size()); c >= 0;
         c = blockIndex(rd16(p.blocks[c], 34), p.blocks.size())) {
        const QByteArray &blk = p.blocks[c];
        if (rd32(blk, 0) == kOperandField)
            *fieldId = rd32(blk, 8);
        else if (rd32(blk, 0) == kOperandValue && rd16(blk, 12) != kTypeList)
            out->values.append(decodePayload(blk, 14, rd16(blk, 12), rd16(blk, 24), *fieldId,
                                             textOf.value(c)));
        if (out->values.size() > 4)
            break;
    }
    return true;
}

} // namespace

namespace AutoFilterCodec {

quint32 toMppDate(const QDateTime &dt)
{
    if (!dt.isValid())
        return kNaDate;
    const qint64 days = kEpoch.daysTo(dt.date());
    const int tenths = (dt.time().hour() * 60 + dt.time().minute()) * 10;
    return (quint32(qMax<qint64>(0, days)) << 16) | quint32(tenths & 0xFFFF);
}

QDateTime fromMppDate(quint32 v)
{
    if (v == kNaDate)
        return {};
    const QDate d = kEpoch.addDays(v >> 16);
    const int minutes = (v & 0xFFFF) == 0xFFFF ? 0 : int(v & 0xFFFF) / 10;
    return QDateTime(d, QTime(0, 0).addSecs(minutes * 60));
}

bool isCurrencyField(quint32 fieldId)
{
    using namespace MppFieldIds;
    const quint16 high = quint16(fieldId >> 16), idx = quint16(fieldId & 0xFFFF);
    auto inCost = [idx](const CostFields &c) {
        return idx == c.cost || idx == c.fixedCost || idx == c.actualCost
            || idx == c.remainingCost || idx == c.costVariance;
    };
    auto inBaselines = [idx](const BaselineSet *sets) {
        for (int i = 0; i < kBaselineCount; ++i)
            if (sets[i].cost != kAbsent && sets[i].cost == idx)
                return true;
        return false;
    };
    auto inCustom = [idx](const QVector<CustomFieldDef> &defs) {
        for (const CustomFieldDef &d : defs)
            if (d.index == idx)
                return d.kind == FieldKind::Currency;
        return false;
    };
    if (high == kTaskHigh)
        return inCost(taskCost) || inBaselines(taskBaselines) || inCustom(taskCustomFields())
            || idx == taskEvm.bcwp || idx == taskEvm.bcws || idx == taskEvm.acwp
            || idx == taskEvm.cv || idx == taskEvm.sv || idx == taskEvm.eac;
    if (high == kResourceHigh)   // + Std. Rate, Ovt. Rate, Cost/Use
        return inCost(resourceCost) || inBaselines(resourceBaselines)
            || inCustom(resourceCustomFields()) || idx == 6 || idx == 7 || idx == 18;
    if (high == kAssignmentHigh)
        return inCost(assignmentCost) || inBaselines(assignmentBaselines)
            || inCustom(assignmentCustomFields());
    return false;
}

QList<ColumnAutoFilter> decode(const QByteArray &item)
{
    QList<ColumnAutoFilter> out;
    const int count = int(rd32(item, 8));
    int o = 16;
    for (int e = 0; e < count && o + 32 <= item.size(); ++e) {
        const int entryStart = o;
        const int entryLength = int(rd32(item, o + 4));
        if (entryLength < 32 || entryStart + entryLength > item.size())
            break;
        ColumnAutoFilter f;
        f.fieldId = rd32(item, o);
        f.filterType = int(rd32(item, o + 8));

        const int b = o + kEntryHeader;                 // criteria base B
        const int blockCount = rd16(item, b + 14);
        const int stringsAt = b + int(rd32(item, b + 16));
        Parsed p;
        for (int i = 0; i < blockCount; ++i) {
            const int at = b + kBlocksStart + i * kBlockSize;
            if (at + kBlockSize > item.size())
                break;
            p.blocks.append(item.mid(at, kBlockSize));
        }
        QHash<int, QString> textOf;
        int s = stringsAt;
        for (int i = 0; i < p.blocks.size(); ++i)
            if (rd32(p.blocks[i], 0) == kOperandValue && rd16(p.blocks[i], 12) == kTypeText)
                textOf.insert(i, rdString(item, s, &s));

        if (!p.blocks.isEmpty()) {
            const quint16 rootOp = rd16(p.blocks[0], 0);
            if (rootOp == kAnd || rootOp == kOr) {
                f.orJoin = rootOp == kOr;
                for (int c = blockIndex(rd16(p.blocks[0], 32), p.blocks.size());
                     c >= 0 && f.conditions.size() < 2;
                     c = blockIndex(rd16(p.blocks[c], 34), p.blocks.size())) {
                    AutoFilterCondition cond;
                    decodeCondition(p, c, textOf, &cond, &f.fieldId);
                    f.conditions.append(cond);
                }
            } else {
                AutoFilterCondition cond;
                decodeCondition(p, 0, textOf, &cond, &f.fieldId);
                if (cond.test != AutoFilterCondition::InList)
                    f.conditions.append(cond);
            }
        }

        o = entryStart + entryLength;
        // An In filter's value list follows its entry (Project lists the
        // values last ticked first; keep the order they were given in).
        if (f.filterType == ColumnAutoFilter::In
            || (!p.blocks.isEmpty() && rd16(p.blocks[0], 0) == AutoFilterCondition::InList)) {
            // Each item: u16 type, 8 payload bytes, and a z-string for text.
            quint16 units = 0;
            for (const QByteArray &blk : std::as_const(p.blocks))
                if (rd32(blk, 0) == kOperandValue && rd16(blk, 12) == kTypeList)
                    units = rd16(blk, 24);
            const int n = rd16(item, o);
            int at = o + 2;
            for (int i = 0; i < n && at + 10 <= item.size(); ++i) {
                const quint16 type = rd16(item, at);
                QString text;
                int next = at + 10;
                if (type == kTypeText)
                    text = rdString(item, at + 10, &next);
                f.list.append(decodePayload(item, at + 2, type, units, f.fieldId, text));
                at = next;
            }
            f.filterType = ColumnAutoFilter::In;
            o = at;
        }
        out.append(f);
    }
    return out;
}

QByteArray encode(const QList<ColumnAutoFilter> &filters, const QDateTime &stamp)
{
    QByteArray entries;
    int written = 0;
    for (const ColumnAutoFilter &f : filters) {
        if (f.fieldId == 0)
            continue;
        const bool isList = f.filterType == ColumnAutoFilter::In;
        if (!isList && f.conditions.isEmpty())
            continue;

        QVector<QByteArray> blocks;
        QStringList texts;
        int textBytes = 0;   // where the next string starts in the strings area
        auto newBlock = [&blocks]() {
            QByteArray blk(kBlockSize, '\0');
            for (int i = 26; i < kBlockSize; ++i)
                blk[i] = char(0xFF);
            blocks.append(blk);
            return int(blocks.size() - 1);
        };
        auto offsetOf = [](int index) { return quint16(kBlocksStart + index * kBlockSize); };
        auto link = [&](int from, int at, int to) { wr16(blocks[from], at, offsetOf(to)); };

        auto addValue = [&](const AutoFilterValue &v) {
            const int i = newBlock();
            QByteArray &blk = blocks[i];
            wr32(blk, 0, kOperandValue);
            wr32(blk, 4, 8);
            quint16 units = 0;
            wr16(blk, 12, encodePayload(v, blk, 14, &units));
            if (v.type == AutoFilterValue::Type::Text) {
                wr16(blk, 26, quint16(textBytes));   // the string's offset in the strings
                texts.append(v.text);
                textBytes += int(v.text.size() + 1) * 2;
            } else if (units) {
                wr16(blk, 24, units);
            }
            return i;
        };
        // A test block with its field and value(s) as children.
        auto addTest = [&](quint16 test, const QList<AutoFilterValue> &values, bool list,
                           bool second = false) {
            const int op = newBlock();
            wr16(blocks[op], 0, test);
            if (!second)
                wr32(blocks[op], 12, 5);
            const int field = newBlock();
            wr32(blocks[field], 0, kOperandField);
            wr32(blocks[field], 4, 3);
            wr32(blocks[field], 8, f.fieldId);
            link(op, 32, field);
            int prev = field;
            if (list) {
                const int v = newBlock();
                wr32(blocks[v], 0, kOperandValue);
                wr32(blocks[v], 4, 8);
                wr16(blocks[v], 12, kTypeList);
                for (const AutoFilterValue &item : f.list) {
                    QByteArray scratch(8, '\0');
                    quint16 units = 0;
                    encodePayload(item, scratch, 0, &units);
                    if (units) {
                        wr16(blocks[v], 24, units);
                        break;
                    }
                }
                link(prev, 34, v);
            }
            for (const AutoFilterValue &value : values) {
                const int v = addValue(value);
                link(prev, 34, v);
                prev = v;
            }
            return op;
        };

        if (isList) {
            addTest(AutoFilterCondition::InList, {}, true);
        } else if (f.conditions.size() == 1) {
            addTest(f.conditions.first().test, f.conditions.first().values, false);
        } else {
            const int root = newBlock();
            wr16(blocks[root], 0, f.orJoin ? kOr : kAnd);
            int prev = -1;
            for (int c = 0; c < f.conditions.size() && c < 2; ++c) {
                const int op = addTest(f.conditions.at(c).test, f.conditions.at(c).values,
                                       false, c > 0);
                if (prev < 0)
                    link(root, 32, op);
                else
                    link(prev, 34, op);
                prev = op;
            }
        }

        const int stringsOffset = kBlocksStart + int(blocks.size()) * kBlockSize;
        QByteArray entry;
        app32(entry, f.fieldId);
        app32(entry, 0);                       // entry length, patched below
        app32(entry, quint32(f.filterType));
        app32(entry, 0xFFFFFFFFu);             // B
        app32(entry, 0);
        // Project writes 0x14 for one test and strings + 12 for an And / Or.
        app32(entry, f.conditions.size() > 1 ? quint32(stringsOffset + 12) : quint32(kBlocksStart));
        app16(entry, 0);
        app16(entry, quint16(blocks.size()));
        app32(entry, quint32(stringsOffset));
        for (const QByteArray &blk : std::as_const(blocks))
            entry += blk;
        for (const QString &t : std::as_const(texts))
            appString(entry, t);
        app32(entry, 0);
        wr32(entry, 4, quint32(entry.size()));
        if (isList) {
            app16(entry, quint16(qMin<qsizetype>(f.list.size(), 65535)));
            for (const AutoFilterValue &item : f.list) {
                QByteArray payload(8, '\0');
                quint16 units = 0;
                app16(entry, encodePayload(item, payload, 0, &units));
                entry += payload;
                if (item.type == AutoFilterValue::Type::Text)
                    appString(entry, item.text);
            }
        }
        entries += entry;
        ++written;
    }

    QByteArray out;
    app32(out, 0);
    app32(out, toMppDate(stamp));
    app32(out, quint32(written));
    app32(out, 0);                             // length, patched below
    out += entries;
    app32(out, 0);
    wr32(out, 12, quint32(out.size()));
    return out;
}

} // namespace AutoFilterCodec
