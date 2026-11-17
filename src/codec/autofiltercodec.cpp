// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/autofiltercodec.h"

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

AutoFilterValue decodeValue(const QByteArray &blk, const QString &text)
{
    AutoFilterValue v;
    const quint16 type = rd16(blk, 12);
    switch (type) {
    case kTypeText:
        v.type = AutoFilterValue::Type::Text;
        v.text = text;
        break;
    case kTypeDateOrDuration: {
        const quint32 raw = rd32(blk, 14);
        const quint16 units = rd16(blk, 24);
        if (units != 0) {
            v.type = AutoFilterValue::Type::Duration;
            v.millis = qint64(raw) * 6000;
            v.durationUnit = units;
        } else {
            v.type = AutoFilterValue::Type::Date;
            v.date = AutoFilterCodec::fromMppDate(raw);
        }
        break;
    }
    case kTypePercent:
        v.type = AutoFilterValue::Type::Percent;
        v.number = rd16(blk, 14);
        break;
    case kTypeDouble:
        if (rd16(blk, 24) == kWorkUnits) {
            v.type = AutoFilterValue::Type::Work;
            v.millis = qRound64(rdDouble(blk, 14) * 60.0);
        } else {
            v.type = AutoFilterValue::Type::Number;
            v.number = rdDouble(blk, 14);
        }
        break;
    case kTypeFlag:
        v.type = AutoFilterValue::Type::Flag;
        v.number = rd16(blk, 14) ? 1 : 0;
        break;
    default:
        v.type = AutoFilterValue::Type::Text;
        v.text = text;
        break;
    }
    return v;
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
            out->values.append(decodeValue(blk, textOf.value(c)));
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
        return 0;
    const qint64 days = kEpoch.daysTo(dt.date());
    const int tenths = (dt.time().hour() * 60 + dt.time().minute()) * 10;
    return (quint32(qMax<qint64>(0, days)) << 16) | quint32(tenths & 0xFFFF);
}

QDateTime fromMppDate(quint32 v)
{
    const QDate d = kEpoch.addDays(v >> 16);
    const int minutes = int(v & 0xFFFF) / 10;
    return QDateTime(d, QTime(0, 0).addSecs(minutes * 60));
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
            const int n = rd16(item, o);
            int at = o + 2;
            QStringList values;
            for (int i = 0; i < n && at + 10 <= item.size(); ++i)
                values.prepend(rdString(item, at + 10, &at));
            f.list = values;
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
            switch (v.type) {
            case AutoFilterValue::Type::Text:
                wr16(blk, 12, kTypeText);
                wr16(blk, 26, quint16(textBytes));   // the string's offset in the strings
                texts.append(v.text);
                textBytes += int(v.text.size() + 1) * 2;
                break;
            case AutoFilterValue::Type::Date:
                wr16(blk, 12, kTypeDateOrDuration);
                wr32(blk, 14, toMppDate(v.date));
                break;
            case AutoFilterValue::Type::Duration:
                wr16(blk, 12, kTypeDateOrDuration);
                wr32(blk, 14, quint32(qMax<qint64>(0, v.millis / 6000)));
                wr16(blk, 24, quint16(v.durationUnit > 0 ? v.durationUnit : 7));
                break;
            case AutoFilterValue::Type::Work:
                wr16(blk, 12, kTypeDouble);
                wrDouble(blk, 14, double(v.millis) / 60.0);
                wr16(blk, 24, kWorkUnits);
                break;
            case AutoFilterValue::Type::Percent:
                wr16(blk, 12, kTypePercent);
                wr16(blk, 14, quint16(qBound(0, qRound(v.number), 65535)));
                break;
            case AutoFilterValue::Type::Flag:
                wr16(blk, 12, kTypeFlag);
                wr16(blk, 14, v.number != 0.0 ? 1 : 0);
                break;
            case AutoFilterValue::Type::Number:
            case AutoFilterValue::Type::Cost:
                wr16(blk, 12, kTypeDouble);
                wrDouble(blk, 14, v.number);
                break;
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
        // Project writes 0x14 for one condition and strings + 12 for two.
        app32(entry, blocks.size() <= 3 ? quint32(kBlocksStart) : quint32(stringsOffset + 12));
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
            for (int i = f.list.size() - 1; i >= 0; --i) {
                app16(entry, kTypeText);
                entry += QByteArray(8, '\0');
                appString(entry, f.list.at(i));
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
