// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "serializer/docserializer.h"
#include "serializer/mpp12serializer.h"
#include "serializer/mpp14serializer.h"

#include "codec/bkndvardata.h"
#include "codec/fielddecoders.h"
#include "codec/propsreader.h"
#include "codec/streamquartet.h"
#include "ole/compoundfile.h"

#include <QHash>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <numeric>

using FormatVersion = MppProject::FormatVersion;

namespace {

// Field type ids for variable-length (string) fields.
constexpr quint16 kFieldName = 1;
constexpr quint16 kFieldInitials = 2;

// ---- fixed-record packing -------------------------------------------------
// These are the scaffold's own record layouts, NOT the real MPP layouts.
// Replace with the reverse-engineered FixedData layouts once fixtures exist.

void putU32(QByteArray &b, quint32 v)
{ char t[4]; qToLittleEndian<quint32>(v, reinterpret_cast<uchar *>(t)); b.append(t, 4); }
void putI32(QByteArray &b, qint32 v) { putU32(b, static_cast<quint32>(v)); }
void putU16(QByteArray &b, quint16 v)
{ char t[2]; qToLittleEndian<quint16>(v, reinterpret_cast<uchar *>(t)); b.append(t, 2); }
void putU8(QByteArray &b, quint8 v) { b.append(static_cast<char>(v)); }

QByteArray packTask(const MppTask &t)
{
    using namespace FieldDecoders;
    QByteArray r;
    putU32(r, static_cast<quint32>(t.uniqueId));
    putU32(r, static_cast<quint32>(t.id));
    putU32(r, static_cast<quint32>(t.outlineLevel));
    putU32(r, encodeTimestampSeconds(t.start));
    putU32(r, encodeTimestampSeconds(t.finish));
    putI32(r, encodeDurationTenthMinutes(t.durationMillis));
    putU16(r, encodePercent(t.percentComplete));
    putU8(r, t.milestone ? 1 : 0);
    putU8(r, t.summary ? 1 : 0);
    return r;   // 28 bytes
}
constexpr int kTaskRecordSize = 28;

MppTask unpackTask(const QByteArray &r)
{
    using namespace FieldDecoders;
    MppTask t;
    quint32 u = 0; qint32 i = 0; quint16 s = 0;
    readU32(r, 0, &u);  t.uniqueId = static_cast<int>(u);
    readU32(r, 4, &u);  t.id = static_cast<int>(u);
    readU32(r, 8, &u);  t.outlineLevel = static_cast<int>(u);
    readU32(r, 12, &u); t.start = decodeTimestampSeconds(u);
    readU32(r, 16, &u); t.finish = decodeTimestampSeconds(u);
    readI32(r, 20, &i); t.durationMillis = decodeDurationTenthMinutes(i);
    readU16(r, 24, &s); t.percentComplete = decodePercent(s);
    t.milestone = r.size() > 26 && r.at(26) != 0;
    t.summary   = r.size() > 27 && r.at(27) != 0;
    return t;
}

QByteArray packResource(const MppResource &res)
{
    QByteArray r;
    putU32(r, static_cast<quint32>(res.uniqueId));
    putU32(r, static_cast<quint32>(res.id));
    putU32(r, static_cast<quint32>(qRound(res.maxUnits * 100000.0)));
    return r;   // 12 bytes
}
constexpr int kResourceRecordSize = 12;

MppResource unpackResource(const QByteArray &r)
{
    MppResource res;
    quint32 u = 0;
    FieldDecoders::readU32(r, 0, &u);  res.uniqueId = static_cast<int>(u);
    FieldDecoders::readU32(r, 4, &u);  res.id = static_cast<int>(u);
    FieldDecoders::readU32(r, 8, &u);  res.maxUnits = static_cast<double>(u) / 100000.0;
    return res;
}

QByteArray packAssignment(const MppAssignment &a)
{
    QByteArray r;
    putU32(r, static_cast<quint32>(a.uniqueId));
    putU32(r, static_cast<quint32>(a.taskUniqueId));
    putU32(r, static_cast<quint32>(a.resourceUniqueId));
    putU32(r, static_cast<quint32>(qRound(a.units * 100000.0)));
    putI32(r, FieldDecoders::encodeDurationTenthMinutes(a.workMillis));
    return r;   // 20 bytes
}
constexpr int kAssignmentRecordSize = 20;

MppAssignment unpackAssignment(const QByteArray &r)
{
    MppAssignment a;
    quint32 u = 0; qint32 i = 0;
    FieldDecoders::readU32(r, 0, &u);  a.uniqueId = static_cast<int>(u);
    FieldDecoders::readU32(r, 4, &u);  a.taskUniqueId = static_cast<int>(u);
    FieldDecoders::readU32(r, 8, &u);  a.resourceUniqueId = static_cast<int>(u);
    FieldDecoders::readU32(r, 12, &u); a.units = static_cast<double>(u) / 100000.0;
    FieldDecoders::readI32(r, 16, &i); a.workMillis = FieldDecoders::decodeDurationTenthMinutes(i);
    return a;
}

QByteArray serializeProps(const MppProject &p, FormatVersion v)
{
    QByteArray b;
    putU16(b, static_cast<quint16>(static_cast<int>(v)));
    b.append(FieldDecoders::encodeUnicodeString(p.title));
    b.append(FieldDecoders::encodeUnicodeString(p.author));
    putU32(b, FieldDecoders::encodeTimestampSeconds(p.startDate));
    putU32(b, FieldDecoders::encodeTimestampSeconds(p.finishDate));
    return b;
}

void deserializeProps(const QByteArray &b, MppProject &p)
{
    using namespace FieldDecoders;
    quint16 ver = 0;
    readU16(b, 0, &ver);
    p.formatVersion = static_cast<FormatVersion>(ver);
    int consumed = 0;
    int off = 2;
    p.title = decodeUnicodeString(b, off, &consumed); off += consumed;
    p.author = decodeUnicodeString(b, off, &consumed); off += consumed;
    quint32 u = 0;
    if (readU32(b, off, &u)) { p.startDate = decodeTimestampSeconds(u); off += 4; }
    if (readU32(b, off, &u)) { p.finishDate = decodeTimestampSeconds(u); off += 4; }
}

StreamQuartet::Streams readQuartet(const CompoundFile &cf, const QString &entity)
{
    StreamQuartet::Streams s;
    s.fixedMeta = cf.readStream({ QStringLiteral("Project"), entity, QStringLiteral("FixedMeta") });
    s.varMeta   = cf.readStream({ QStringLiteral("Project"), entity, QStringLiteral("VarMeta") });
    s.fixedData = cf.readStream({ QStringLiteral("Project"), entity, QStringLiteral("FixedData") });
    s.var2Data  = cf.readStream({ QStringLiteral("Project"), entity, QStringLiteral("Var2Data") });
    return s;
}

// The main entity data of a real .mpp lives under a storage named "   114"
// (three leading spaces), with sub-storages TBkndTask, TBkndRsc, etc.
const QString kDataStorage = QStringLiteral("   114");

bool isRealMpp(const CompoundFile &cf)
{
    return cf.hasStorage({ kDataStorage, QStringLiteral("TBkndTask") });
}

// Fixed-data offsets of the task fields we read, recovered from the task field
// map. Field indices are MPPTaskField.FIELD_ARRAY positions.
struct TaskFixedOffsets {
    int uniqueId = -1;   // index 86  (block 0)
    int start = -1;      // index 35  (block 0, auto-scheduled)
    int finish = -1;     // index 36  (block 0, auto-scheduled)
    int duration = -1;   // index 29  (block 0, mode-independent)
    int start1 = -1;     // index 1283 (block 1 / Fixed2Data, manual-scheduled)
    int finish1 = -1;    // index 1284 (block 1 / Fixed2Data, manual-scheduled)
    int id = -1;         // index 23  (block 0)
    int percent = -1;    // index 32  (block 0)
    int outline = -1;    // index 249 (block 0)
    // Minimum block size to treat a block as a real (non-null) task record.
    int block0Needed() const { return qMax(uniqueId, duration) + 4; }
};

// Parse the task field map (28-byte entries) per MPXJ createFieldMap, tracking
// the stateful fixed-data block index, and pick out the block-0 fixed offsets.
TaskFixedOffsets parseTaskFixedOffsets(const QByteArray &fm)
{
    using namespace FieldDecoders;
    TaskFixedOffsets off;
    int lastBlockOffset = 0, blockIndex = 0;
    auto setOnce = [](int &slot, int v) { if (slot < 0) slot = v; };

    for (int i = 0; i + 28 <= fm.size(); i += 28) {
        quint32 typeValue = 0;
        quint16 dataBlockOffset = 0, category = 0;
        readU32(fm, i + 12, &typeValue);
        readU16(fm, i + 4, &dataBlockOffset);
        readU16(fm, i + 20, &category);

        const bool fixed = (category != 0x0B && category != 0x64 && dataBlockOffset != 0xFFFF);
        int thisBlock = 0;
        if (fixed) {
            if (dataBlockOffset < lastBlockOffset)
                ++blockIndex;
            lastBlockOffset = dataBlockOffset;
            thisBlock = blockIndex;
        }
        if (!fixed || (typeValue >> 16) != 0x0B40)
            continue;
        const quint16 idx = typeValue & 0xFFFF;
        if (thisBlock == 0) {
            switch (idx) {
            case 86: setOnce(off.uniqueId, dataBlockOffset); break;
            case 35: setOnce(off.start, dataBlockOffset); break;
            case 36: setOnce(off.finish, dataBlockOffset); break;
            case 29: setOnce(off.duration, dataBlockOffset); break;
            case 23: setOnce(off.id, dataBlockOffset); break;
            case 32: setOnce(off.percent, dataBlockOffset); break;
            case 249: setOnce(off.outline, dataBlockOffset); break;
            default: break;
            }
        } else if (thisBlock == 1) {
            switch (idx) {
            case 1283: setOnce(off.start1, dataBlockOffset); break;   // "Task Start"
            case 1284: setOnce(off.finish1, dataBlockOffset); break;  // "Task Finish"
            default: break;
            }
        }
    }
    return off;
}

// Split a fixed-data stream into per-item blocks using its FixedMeta (16-byte
// header, fixed-size items whose +4 field is the block's offset into the data).
// The returned vector is indexed by item position (empty entry == no block) so
// that block-0 (FixedData) and block-1 (Fixed2Data) stay aligned. MPXJ FixedData.
QVector<QByteArray> readFixedBlocks(const QByteArray &meta, const QByteArray &data, int itemSize)
{
    using namespace FieldDecoders;
    constexpr int kHeader = 16;
    QVector<QByteArray> blocks;
    if (itemSize <= 0)
        return blocks;
    const int count = (meta.size() - kHeader) / itemSize;
    blocks.reserve(count);
    for (int loop = 0; loop < count; ++loop) {
        quint32 itemOffset = 0, nextOffset = static_cast<quint32>(data.size());
        readU32(meta, kHeader + loop * itemSize + 4, &itemOffset);
        if (loop + 1 < count)
            readU32(meta, kHeader + (loop + 1) * itemSize + 4, &nextOffset);
        if (itemOffset >= static_cast<quint32>(data.size()) || nextOffset <= itemOffset)
            blocks.append(QByteArray());   // keep index alignment
        else
            blocks.append(data.mid(static_cast<int>(itemOffset),
                                   static_cast<int>(nextOffset - itemOffset)));
    }
    return blocks;
}

// Generic: map field index -> block-0 fixed-data offset for an entity field map
// (28-byte entries), keeping only entries whose high word matches the entity
// (task 0x0B40, resource 0x0C40, assignment 0x0F40) and that live in block 0.
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

// Fetch an entity field map from the project Props (primary key, else fallback).
QByteArray fieldMapBytes(const PropsReader &props, quint32 key1, quint32 key2)
{
    QByteArray b = props.value(key1);
    return b.isEmpty() ? props.value(key2) : b;
}

double readDoubleLE(const QByteArray &d, int off)
{
    if (off < 0 || off + 8 > d.size())
        return 0.0;
    quint64 bits = qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(d.constData()) + off);
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// Resources: names/initials from var data (NAME=1, INITIALS=2), UID/ID/MaxUnits
// from FixedData (resource field map 0x00020015, FixedMeta item size 37).
void readRealResources(const CompoundFile &cf, MppProject &out, const PropsReader &props)
{
    const QString rsc = QStringLiteral("TBkndRsc");
    if (!cf.hasStorage({ kDataStorage, rsc }))
        return;

    BkndVarData v;
    v.parse(cf.readStream({ kDataStorage, rsc, QStringLiteral("VarMeta") }),
            cf.readStream({ kDataStorage, rsc, QStringLiteral("Var2Data") }));
    QHash<int, QString> names, initials;
    for (const auto &e : v.stringsForType(1)) names.insert(int(e.uniqueId), e.value);
    for (const auto &e : v.stringsForType(2)) initials.insert(int(e.uniqueId), e.value);

    const QHash<quint16, int> off =
        block0FixedOffsets(fieldMapBytes(props, 0x00020015u, 0x03000015u), 0x0C40);
    const int uidOff = off.value(27, -1), idOff = off.value(0, -1), maxOff = off.value(4, -1);
    if (uidOff < 0)
        return;

    const QVector<QByteArray> blocks = readFixedBlocks(
        cf.readStream({ kDataStorage, rsc, QStringLiteral("FixedMeta") }),
        cf.readStream({ kDataStorage, rsc, QStringLiteral("FixedData") }), 37);
    QSet<int> seen;
    for (const QByteArray &b : blocks) {
        quint32 uid = 0;
        if (!FieldDecoders::readU32(b, uidOff, &uid) || seen.contains(int(uid)))
            continue;
        if (!names.contains(int(uid)))   // only real, named resources
            continue;
        seen.insert(int(uid));
        MppResource r;
        r.uniqueId = int(uid);
        r.name = names.value(int(uid));
        r.initials = initials.value(int(uid));
        quint32 v32 = 0;
        if (idOff >= 0 && FieldDecoders::readU32(b, idOff, &v32))
            r.id = int(v32);
        if (maxOff >= 0)
            r.maxUnits = readDoubleLE(b, maxOff);   // MAX_UNITS is an 8-byte double (1.0 == 100%)
        out.resources.append(r);
    }
}

// Assignments: link task<->resource with units/work, all from FixedData
// (assignment field map 0x00020017, FixedMeta item size 34).
void readRealAssignments(const CompoundFile &cf, MppProject &out, const PropsReader &props)
{
    const QString assn = QStringLiteral("TBkndAssn");
    if (!cf.hasStorage({ kDataStorage, assn }))
        return;

    const QHash<quint16, int> off =
        block0FixedOffsets(fieldMapBytes(props, 0x00020017u, 0x03000017u), 0x0F40);
    const int uidOff = off.value(0, -1), taskOff = off.value(1, -1), resOff = off.value(2, -1),
              unitsOff = off.value(7, -1), workOff = off.value(8, -1);
    if (taskOff < 0 || resOff < 0)
        return;

    const QVector<QByteArray> blocks = readFixedBlocks(
        cf.readStream({ kDataStorage, assn, QStringLiteral("FixedMeta") }),
        cf.readStream({ kDataStorage, assn, QStringLiteral("FixedData") }), 34);
    QSet<int> seen;
    for (const QByteArray &b : blocks) {
        quint32 taskUid = 0, resUid = 0;
        if (!FieldDecoders::readU32(b, taskOff, &taskUid)
            || !FieldDecoders::readU32(b, resOff, &resUid))
            continue;
        MppAssignment a;
        quint32 v32 = 0;
        if (uidOff >= 0 && FieldDecoders::readU32(b, uidOff, &v32))
            a.uniqueId = int(v32);
        if (seen.contains(a.uniqueId))
            continue;
        seen.insert(a.uniqueId);
        a.taskUniqueId = int(taskUid);
        a.resourceUniqueId = int(resUid);
        if (unitsOff >= 0)
            a.units = readDoubleLE(b, unitsOff);          // units (1.0 == 100%)
        if (workOff >= 0)
            a.workMillis = FieldDecoders::decodeDurationTenthMinutes(
                static_cast<qint32>(readDoubleLE(b, workOff)));   // work as tenths-of-minute
        out.assignments.append(a);
    }
}

// Reads what we have so far reverse-engineered from real files: task names from
// "   114/TBkndTask" (VarMeta field code 6 -> Var2Data UTF-16 strings). Other
// fields (UID, dates, duration, resources) are the next RE increments.
bool readRealMpp(const CompoundFile &cf, MppProject &out, MppProject::FormatVersion ver)
{
    out = MppProject();
    out.formatVersion = ver;

    const QString task = QStringLiteral("TBkndTask");

    // Names come from var data field 14 (TaskField.NAME); one task per unique id.
    BkndVarData taskVars;
    taskVars.parse(cf.readStream({ kDataStorage, task, QStringLiteral("VarMeta") }),
                   cf.readStream({ kDataStorage, task, QStringLiteral("Var2Data") }));

    QHash<int, int> pos;   // unique id -> index into out.tasks
    for (const BkndVarData::Entry &e : taskVars.stringsForType(BkndVarData::FieldTaskName)) {
        const int uid = int(e.uniqueId);
        if (e.value.isEmpty() || pos.contains(uid))
            continue;
        pos.insert(uid, out.tasks.size());
        MppTask t;
        t.uniqueId = uid;
        t.name = e.value;
        out.tasks.append(t);
    }

    // Start / Finish / Duration come from FixedData, located via the task field
    // map in "   114/Props" (TASK_FIELD_MAP=0x00020014, fallback 0x03000014).
    PropsReader props;
    props.parse(cf.readStream({ kDataStorage, QStringLiteral("Props") }));
    QByteArray fmBytes = props.value(0x00020014u);
    if (fmBytes.isEmpty())
        fmBytes = props.value(0x03000014u);
    const TaskFixedOffsets off = parseTaskFixedOffsets(fmBytes);

    if (off.uniqueId >= 0) {
        const int needed = off.block0Needed() + 4;
        const QByteArray fixedMeta = cf.readStream({ kDataStorage, task, QStringLiteral("FixedMeta") });
        const QVector<QByteArray> blocks0 = readFixedBlocks(
            fixedMeta,
            cf.readStream({ kDataStorage, task, QStringLiteral("FixedData") }), 47);

        // Fixed2Data (block 1) holds manual-scheduled dates. Its FixedMeta item
        // size is adaptive (MPXJ tries 92..96); derive it from the item count.
        const QByteArray meta2 = cf.readStream({ kDataStorage, task, QStringLiteral("Fixed2Meta") });
        const QByteArray data2 = cf.readStream({ kDataStorage, task, QStringLiteral("Fixed2Data") });
        const int item2 = (!blocks0.isEmpty() && meta2.size() > 16)
                              ? (meta2.size() - 16) / blocks0.size()
                              : 0;
        const QVector<QByteArray> blocks1 =
            (item2 >= 88 && item2 <= 100) ? readFixedBlocks(meta2, data2, item2)
                                          : QVector<QByteArray>();

        // Effective date: the manual field (block 1) if present, else the
        // auto/computed field (block 0). Matches MS Project's displayed dates.
        auto effectiveDate = [&](int loop, int b1Off, int b0Off, const QByteArray &b0) {
            if (b1Off >= 0 && loop < blocks1.size()) {
                const QDateTime d = FieldDecoders::decodeMppTimestamp(blocks1.at(loop), b1Off);
                if (d.isValid())
                    return d;
            }
            return b0Off >= 0 ? FieldDecoders::decodeMppTimestamp(b0, b0Off) : QDateTime();
        };

        QSet<int> filled;   // first full block per unique id wins (matches MPXJ)
        for (int loop = 0; loop < blocks0.size(); ++loop) {
            const QByteArray &b0 = blocks0.at(loop);
            if (b0.size() < needed)
                continue;   // skip null/partial task blocks
            quint32 uid32 = 0;
            FieldDecoders::readU32(b0, off.uniqueId, &uid32);
            const auto it = pos.constFind(int(uid32));
            if (it == pos.constEnd() || filled.contains(int(uid32)))
                continue;
            filled.insert(int(uid32));
            MppTask &t = out.tasks[it.value()];
            t.start = effectiveDate(loop, off.start1, off.start, b0);
            t.finish = effectiveDate(loop, off.finish1, off.finish, b0);
            quint32 u32v = 0;
            quint16 u16v = 0;
            if (off.duration >= 0 && FieldDecoders::readU32(b0, off.duration, &u32v))
                t.durationMillis = FieldDecoders::decodeDurationTenthMinutes(static_cast<qint32>(u32v));
            if (off.id >= 0 && FieldDecoders::readU32(b0, off.id, &u32v))
                t.id = static_cast<int>(u32v);
            if (off.percent >= 0 && FieldDecoders::readU16(b0, off.percent, &u16v))
                t.percentComplete = FieldDecoders::decodePercent(u16v);
            if (off.outline >= 0 && FieldDecoders::readU16(b0, off.outline, &u16v))
                t.outlineLevel = static_cast<int>(u16v);

            // MILESTONE is a bit flag in the 47-byte FixedMeta item (MPXJ
            // *_TASK_META_DATA_BIT_FLAGS). Project 2013/2016: int at meta offset
            // 10, mask 0x02. (Project 2010 used offset 8, mask 0x20.)
            quint32 metaFlags = 0;
            if (FieldDecoders::readU32(fixedMeta, 16 + loop * 47 + 10, &metaFlags))
                t.milestone = (metaFlags & 0x02u) != 0;
        }

        // SUMMARY is derived from the outline hierarchy: a task is a summary if
        // a following task (in ID order) sits one outline level deeper, or if it
        // is the project summary row (outline level 0).
        QVector<int> byId(out.tasks.size());
        std::iota(byId.begin(), byId.end(), 0);
        std::sort(byId.begin(), byId.end(),
                  [&](int a, int b) { return out.tasks[a].id < out.tasks[b].id; });
        for (int i = 0; i < byId.size(); ++i) {
            MppTask &cur = out.tasks[byId[i]];
            const bool hasChild = (i + 1 < byId.size())
                && out.tasks[byId[i + 1]].outlineLevel > cur.outlineLevel;
            cur.summary = hasChild || cur.outlineLevel == 0;
        }
    }

    readRealResources(cf, out, props);
    readRealAssignments(cf, out, props);
    return true;
}

void writeQuartet(CompoundFile &cf, const QString &entity, const StreamQuartet::Streams &s)
{
    cf.addStream({ QStringLiteral("Project"), entity, QStringLiteral("FixedMeta") }, s.fixedMeta);
    cf.addStream({ QStringLiteral("Project"), entity, QStringLiteral("VarMeta") },   s.varMeta);
    cf.addStream({ QStringLiteral("Project"), entity, QStringLiteral("FixedData") }, s.fixedData);
    cf.addStream({ QStringLiteral("Project"), entity, QStringLiteral("Var2Data") },  s.var2Data);
}

} // namespace

// ---------------------------------------------------------------------------

FormatVersion DocSerializer::detectVersion(const CompoundFile &cf)
{
    if (!cf.isValid())
        return FormatVersion::Unknown;

    // Real MS Project files carry a top-level "Props<NN>" stream whose suffix is
    // the format version (confirmed against fixtures: "Props14" -> MPP.14).
    if (cf.hasStream({ QStringLiteral("Props14") }))
        return FormatVersion::Mpp14;
    if (cf.hasStream({ QStringLiteral("Props12") }))
        return FormatVersion::Mpp12;

    // The scaffold's own written files store the version in Project/Props.
    const QByteArray props = cf.readStream({ QStringLiteral("Project"), QStringLiteral("Props") });
    quint16 ver = 0;
    if (FieldDecoders::readU16(props, 0, &ver)) {
        if (ver == 12) return FormatVersion::Mpp12;
        if (ver == 14) return FormatVersion::Mpp14;
    }
    return FormatVersion::Unknown;
}

std::unique_ptr<DocSerializer> DocSerializer::create(FormatVersion v)
{
    switch (v) {
    case FormatVersion::Mpp12: return std::make_unique<Mpp12Serializer>();
    case FormatVersion::Mpp14: return std::make_unique<Mpp14Serializer>();
    default: return nullptr;
    }
}

bool DocSerializer::read(const CompoundFile &cf, MppProject &out, QString *error) const
{
    if (!cf.isValid()) {
        if (error) *error = QStringLiteral("invalid compound file");
        return false;
    }

    // Real Microsoft Project files: read what we have reverse-engineered so far.
    if (isRealMpp(cf))
        return readRealMpp(cf, out, version());

    // Otherwise fall back to the scaffold's own self-consistent format.
    if (!cf.hasStorage({ QStringLiteral("Project") })) {
        if (error) *error = QStringLiteral("missing 'Project' storage");
        return false;
    }

    out = MppProject();
    deserializeProps(cf.readStream({ QStringLiteral("Project"), QStringLiteral("Props") }), out);

    QString qerr;
    const StreamQuartet tasks = StreamQuartet::decode(readQuartet(cf, QStringLiteral("Task")), &qerr);
    for (int i = 0; i < tasks.fixedRecords.size(); ++i) {
        MppTask t = unpackTask(tasks.fixedRecords.at(i));
        for (const auto &ve : tasks.varEntries)
            if (static_cast<int>(ve.itemIndex) == i && ve.fieldType == kFieldName)
                t.name = QString::fromUtf16(reinterpret_cast<const char16_t *>(ve.data.constData()),
                                            ve.data.size() / 2);
        out.tasks.append(t);
    }

    const StreamQuartet res = StreamQuartet::decode(readQuartet(cf, QStringLiteral("Resource")), &qerr);
    for (int i = 0; i < res.fixedRecords.size(); ++i) {
        MppResource r = unpackResource(res.fixedRecords.at(i));
        for (const auto &ve : res.varEntries) {
            if (static_cast<int>(ve.itemIndex) != i)
                continue;
            const QString s = QString::fromUtf16(
                reinterpret_cast<const char16_t *>(ve.data.constData()), ve.data.size() / 2);
            if (ve.fieldType == kFieldName) r.name = s;
            else if (ve.fieldType == kFieldInitials) r.initials = s;
        }
        out.resources.append(r);
    }

    const StreamQuartet asn = StreamQuartet::decode(readQuartet(cf, QStringLiteral("Assignment")), &qerr);
    for (const QByteArray &rec : asn.fixedRecords)
        out.assignments.append(unpackAssignment(rec));

    return true;
}

bool DocSerializer::write(const MppProject &in, CompoundFile &cf, QString *error) const
{
    Q_UNUSED(error);
    cf.addStream({ QStringLiteral("Project"), QStringLiteral("Props") },
                 serializeProps(in, version()));

    StreamQuartet tasks;
    tasks.recordSize = kTaskRecordSize;
    for (int i = 0; i < in.tasks.size(); ++i) {
        const MppTask &t = in.tasks.at(i);
        tasks.fixedRecords.append(packTask(t));
        if (!t.name.isEmpty())
            tasks.varEntries.append({ static_cast<quint32>(i), kFieldName,
                                      QByteArray(reinterpret_cast<const char *>(t.name.utf16()),
                                                 t.name.size() * 2) });
    }
    writeQuartet(cf, QStringLiteral("Task"), tasks.encode());

    StreamQuartet res;
    res.recordSize = kResourceRecordSize;
    for (int i = 0; i < in.resources.size(); ++i) {
        const MppResource &r = in.resources.at(i);
        res.fixedRecords.append(packResource(r));
        if (!r.name.isEmpty())
            res.varEntries.append({ static_cast<quint32>(i), kFieldName,
                                    QByteArray(reinterpret_cast<const char *>(r.name.utf16()),
                                               r.name.size() * 2) });
        if (!r.initials.isEmpty())
            res.varEntries.append({ static_cast<quint32>(i), kFieldInitials,
                                    QByteArray(reinterpret_cast<const char *>(r.initials.utf16()),
                                               r.initials.size() * 2) });
    }
    writeQuartet(cf, QStringLiteral("Resource"), res.encode());

    StreamQuartet asn;
    asn.recordSize = kAssignmentRecordSize;
    for (const MppAssignment &a : in.assignments)
        asn.fixedRecords.append(packAssignment(a));
    writeQuartet(cf, QStringLiteral("Assignment"), asn.encode());

    return true;
}
