// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "serializer/mpp14writer.h"

#include "codec/fielddecoders.h"
#include "codec/fieldmap.h"
#include "codec/mppfieldids.h"
#include "codec/propsreader.h"
#include "codec/viewformat.h"
#include "ole/compoundfile.h"
#include "serializer/mpp14manifest.h"

#include <QFile>
#include <QHash>
#include <QSet>
#include <QUuid>
#include <QVector>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <functional>

// The .qrc that carries the template lives in the library; make sure its
// registration ran even when the objects are linked statically (tests).
static void scheduleioEnsureResources()
{
    Q_INIT_RESOURCE(scheduleio);
}

namespace {

using namespace FieldDecoders;
using FieldMap::EntityFieldLoc;

// ---- record geometry, recovered from Project 2016 files (DECODING_NOTES.md) --
constexpr int kTaskRecSize = 202, kTaskMetaItem = 47, kTaskF2Block = 64, kTaskF2MetaItem = 96;
constexpr int kRscRecSize = 172, kRscMetaItem = 37, kRscF2Block = 40, kRscF2MetaItem = 51;
constexpr int kAssnRecSize = 110, kAssnMetaItem = 34, kAssnF2Block = 48, kAssnF2MetaItem = 53;
constexpr int kConsRecSize = 20, kConsMetaItem = 10, kConsF2Block = 48, kConsF2MetaItem = 9;
constexpr int kCalRecSize = 12, kCalMetaItem = 10, kCalF2Block = 48, kCalF2MetaItem = 10;

// Manual-scheduled ("Task Start"/"Task Finish") field indices in fixed block 1.
constexpr quint16 kTaskStartManual = 1283, kTaskFinishManual = 1284;
// Var-data keys: entity NAME / NOTES / resource INITIALS / cost-rate tables A..E.
constexpr quint16 kTaskName = 14, kTaskNotes = 15;
constexpr quint16 kRscName = 1, kRscInitials = 2, kRscNotes = 20;
constexpr quint16 kAssnNotes = 71;
constexpr quint16 kCalName = 1, kCalData = 8;
constexpr quint16 kCalHigh = 0x0D40;
constexpr quint16 kCostRateVarKey[5] = { 61, 62, 63, 64, 65 };
constexpr quint16 kAvailabilityVarKey = 276;   // MPXJ ResourceField.AVAILABILITY_DATA

const QString kDataStorage = QStringLiteral("   114");

// Namespace UUID for deterministic (v5) entity GUIDs, so a re-save of the same
// model is byte-identical. Arbitrary but fixed.
const QUuid kGuidNs("{7a1f5b7e-30d2-4c8a-9a51-6e6c1f0e5a2d}");

// Project's sentinel GUID for the synthetic "unassigned resource" used by
// task-only assignments. Unlike a normal resource GUID it has no resource row.
const QByteArray kUnassignedResourceGuid = QByteArray::fromHex(
    "788bcba08c2a6d4300000000000000ff");

QByteArray guidFor(const char *kind, int uid)
{
    return encodeGuid(QUuid::createUuidV5(
        kGuidNs, QStringLiteral("scheduleio-%1-%2").arg(QLatin1String(kind)).arg(uid)));
}

qint64 amountAtPercent(qint64 total, quint16 percent)
{
    total = qMax<qint64>(0, total);
    return qRound64(double(total) * double(percent) / 100.0);
}

// ---- little-endian appends / pokes ------------------------------------------

void appU16(QByteArray &b, quint16 v)
{ char t[2]; qToLittleEndian<quint16>(v, reinterpret_cast<uchar *>(t)); b.append(t, 2); }
void appU32(QByteArray &b, quint32 v)
{ char t[4]; qToLittleEndian<quint32>(v, reinterpret_cast<uchar *>(t)); b.append(t, 4); }
quint32 readLeU32(const QByteArray &b, int off)
{
    return off >= 0 && off + 4 <= b.size()
        ? qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + off)) : 0;
}
void appDouble(QByteArray &b, double v)
{ quint64 bits; memcpy(&bits, &v, 8); char t[8];
  qToLittleEndian<quint64>(bits, reinterpret_cast<uchar *>(t)); b.append(t, 8); }

bool pokeU16(QByteArray &b, int off, quint16 v)
{
    if (off < 0 || off + 2 > b.size())
        return false;
    qToLittleEndian<quint16>(v, reinterpret_cast<uchar *>(b.data() + off));
    return true;
}
bool pokeU32(QByteArray &b, int off, quint32 v)
{
    if (off < 0 || off + 4 > b.size())
        return false;
    qToLittleEndian<quint32>(v, reinterpret_cast<uchar *>(b.data() + off));
    return true;
}
bool pokeDouble(QByteArray &b, int off, double v)
{
    if (off < 0 || off + 8 > b.size())
        return false;
    quint64 bits;
    memcpy(&bits, &v, 8);
    qToLittleEndian<quint64>(bits, reinterpret_cast<uchar *>(b.data() + off));
    return true;
}
bool pokeBytes(QByteArray &b, int off, const QByteArray &src)
{
    if (off < 0 || off + src.size() > b.size())
        return false;
    memcpy(b.data() + off, src.constData(), size_t(src.size()));
    return true;
}

QByteArray utf16zBytes(const QString &s)
{
    QByteArray b(reinterpret_cast<const char *>(s.utf16()), s.size() * 2);
    b.append(char(0));
    b.append(char(0));
    return b;
}

// ---- stream builders ---------------------------------------------------------
// Header layouts confirmed against real files and against the WINPROJ save path
// (FixedMeta: FUN_1408af414 writes [magic 0xFADFADBA][4][...]; VarMeta:
// FUN_1408af56c writes a 24-byte header with word 1 forced to 0).

constexpr quint32 kMetaMagic = 0xFADFADBAu;

// FixedMeta + FixedData (also used for Fixed2Meta + Fixed2Data): fixed-size meta
// items [u32 flags][u32 offset][tail...] pointing at concatenated data blocks.
class FixedBuilder
{
public:
    explicit FixedBuilder(int metaItemSize) : m_itemSize(metaItemSize) {}

    // Copy pre-built items + data verbatim (the template's placeholder rows).
    void addRaw(const QByteArray &metaItems, const QByteArray &data)
    {
        m_meta += metaItems;
        m_data += data;
        m_items += metaItems.size() / m_itemSize;
    }

    // Append one record; `tail` is the meta item's bytes past [flags][offset]
    // (zero-padded / truncated to the item size).
    void addItem(quint32 flags, const QByteArray &record, const QByteArray &tail = QByteArray())
    {
        QByteArray item;
        appU32(item, flags);
        appU32(item, quint32(m_data.size()));
        item += tail.left(m_itemSize - 8);
        if (item.size() < m_itemSize)
            item += QByteArray(m_itemSize - item.size(), '\0');
        m_meta += item;
        m_data += record;
        ++m_items;
    }

    QByteArray meta() const
    {
        QByteArray b;
        appU32(b, kMetaMagic);
        appU32(b, 4);                       // constant, per FUN_1408af414
        appU32(b, quint32(m_items));
        appU32(b, quint32(m_data.size()));  // companion data-stream size
        b += m_meta;
        return b;
    }
    QByteArray data() const { return m_data; }

private:
    int m_itemSize;
    int m_items = 0;
    QByteArray m_meta;
    QByteArray m_data;
};

// VarMeta + Var2Data: 12-byte records [u32 uid][u32 offset][u16 type][u16 high]
// pointing at length-prefixed blobs in the pool.
class VarBuilder
{
public:
    void add(quint32 uid, quint16 type, quint16 high, const QByteArray &payload)
    {
        Rec r{ uid, quint32(m_pool.size()), type, high };
        m_recs.append(r);
        appU32(m_pool, quint32(payload.size()));
        m_pool += payload;
    }

    QByteArray meta() const
    {
        QByteArray b;
        appU32(b, kMetaMagic);
        appU32(b, 0);                       // word 1 is 0, per FUN_1408af56c
        appU32(b, quint32(m_recs.size()));
        appU32(b, 0);
        appU32(b, 0);
        appU32(b, quint32(m_pool.size())); // Var2Data size
        for (const Rec &r : m_recs) {
            appU32(b, r.uid);
            appU32(b, r.offset);
            appU16(b, r.type);
            appU16(b, r.high);
        }
        return b;
    }
    QByteArray data() const { return m_pool; }

private:
    struct Rec { quint32 uid; quint32 offset; quint16 type; quint16 high; };
    QList<Rec> m_recs;
    QByteArray m_pool;
};

// ---- generic field placement -------------------------------------------------
// Writes one entity's fields where the (template's) field map says they live:
// FixedData block 0, Fixed2Data block 1, or a Var2Data blob keyed by the index.
// The exact mirror of the reader's fillCostBaselineCustom/locate resolution.
struct EntitySink
{
    const QHash<quint16, EntityFieldLoc> *loc = nullptr;
    QByteArray *b0 = nullptr;    // block-0 fixed record
    QByteArray *b1 = nullptr;    // block-1 (Fixed2Data) record, tasks only
    QList<QPair<quint16, QByteArray>> varBlobs;   // collected var payloads, by key
    quint32 uid = 0;

    // Resolve a fixed destination; returns nullptr when the field is var/absent.
    QByteArray *fixedDest(quint16 idx, int size, int *off) const
    {
        const auto it = loc->constFind(idx);
        if (it == loc->constEnd())
            return nullptr;
        const EntityFieldLoc &L = it.value();
        QByteArray *dst = (L.block == 0) ? b0 : (L.block == 1) ? b1 : nullptr;
        if (!dst || L.offset < 0 || L.offset + size > dst->size())
            return nullptr;
        *off = L.offset;
        return dst;
    }
    bool isVar(quint16 idx) const
    {
        const auto it = loc->constFind(idx);
        return it != loc->constEnd() && it->block < 0 && it->var;
    }

    // `always`: write even when the value is the default (fixed slots are free;
    // var blobs for default values are only written when the caller insists).
    void putDouble(quint16 idx, double v, bool always = false)
    {
        if (idx == MppFieldIds::kAbsent)
            return;
        int off = 0;
        if (QByteArray *dst = fixedDest(idx, 8, &off)) {
            pokeDouble(*dst, off, v);
        } else if (isVar(idx) && (always || v != 0.0)) {
            QByteArray b;
            appDouble(b, v);
            varBlobs.append({ idx, b });
        }
    }
    void putU16(quint16 idx, quint16 v, bool always = true)
    {
        if (idx == MppFieldIds::kAbsent)
            return;
        int off = 0;
        if (QByteArray *dst = fixedDest(idx, 2, &off)) {
            pokeU16(*dst, off, v);
        } else if (isVar(idx) && (always || v != 0)) {
            QByteArray b;
            appU16(b, v);
            varBlobs.append({ idx, b });
        }
    }
    void putU32(quint16 idx, quint32 v, bool always = true)
    {
        if (idx == MppFieldIds::kAbsent)
            return;
        int off = 0;
        if (QByteArray *dst = fixedDest(idx, 4, &off)) {
            pokeU32(*dst, off, v);
        } else if (isVar(idx) && (always || v != 0)) {
            QByteArray b;
            appU32(b, v);
            varBlobs.append({ idx, b });
        }
    }
    void putDate(quint16 idx, const QDateTime &dt)
    {
        if (idx == MppFieldIds::kAbsent)
            return;
        int off = 0;
        if (QByteArray *dst = fixedDest(idx, 4, &off))
            pokeU32(*dst, off, encodeMppTimestamp(dt));
        else if (isVar(idx) && dt.isValid()) {
            QByteArray b;
            appU32(b, encodeMppTimestamp(dt));
            varBlobs.append({ idx, b });
        }
    }
    void putDuration(quint16 idx, qint64 millis, bool always = true)
    {
        putU32(idx, quint32(encodeDurationTenthMinutes(millis)), always || millis != 0);
    }
    void putWork(quint16 idx, qint64 millis, bool always = false)
    {
        putDouble(idx, encodeWorkDouble(millis), always || millis != 0);
    }
    void putVarBlob(quint16 idx, const QByteArray &payload)
    {
        varBlobs.append({ idx, payload });
    }
};

// Baselines / custom fields (shared by all three entities).
void putBaselines(EntitySink &sink, const QList<schedule::Baseline> &baselines,
                  const MppFieldIds::BaselineSet *sets)
{
    for (const schedule::Baseline &b : baselines) {
        if (b.number < 0 || b.number >= MppFieldIds::kBaselineCount)
            continue;
        const MppFieldIds::BaselineSet &bs = sets[b.number];
        // Cost/work/duration are written even when zero: the reader treats the
        // mere presence of a baseline field as "this baseline exists", so a
        // saved-but-empty baseline must keep its (zero-valued) blobs.
        sink.putDouble(bs.cost, b.cost, true);
        sink.putWork(bs.work, b.workMillis, true);
        sink.putDate(bs.start, b.start);
        sink.putDate(bs.finish, b.finish);
        sink.putDuration(bs.duration, b.durationMillis, true);
    }
}

void putCustomFields(EntitySink &sink, const QList<schedule::CustomField> &fields,
                     quint16 highWord, const QVector<MppFieldIds::CustomFieldDef> &defs)
{
    QHash<quint16, MppFieldIds::FieldKind> kind;
    for (const MppFieldIds::CustomFieldDef &d : defs)
        kind.insert(d.index, d.kind);

    for (const schedule::CustomField &c : fields) {
        if (quint16(quint32(c.fieldId) >> 16) != highWord)
            continue;
        const quint16 idx = quint16(c.fieldId & 0xFFFF);
        const MppFieldIds::FieldKind k = kind.value(idx, MppFieldIds::FieldKind::String);
        switch (k) {
        case MppFieldIds::FieldKind::String: {
            const QString s = c.value.toString();
            if (!s.isEmpty())
                sink.putVarBlob(idx, utf16zBytes(s));
            break;
        }
        case MppFieldIds::FieldKind::Number:
        case MppFieldIds::FieldKind::Currency:
            sink.putDouble(idx, c.value.toDouble(), true);
            break;
        case MppFieldIds::FieldKind::DateTime:
            sink.putDate(idx, c.value.toDateTime());
            break;
        case MppFieldIds::FieldKind::Duration:
            sink.putDuration(idx, c.value.toLongLong());
            break;
        case MppFieldIds::FieldKind::Bool: {
            QByteArray b;
            b.append(c.value.toBool() ? char(1) : char(0));
            sink.putVarBlob(idx, b);
            break;
        }
        }
    }
}

// Flush an EntitySink's var payloads into the entity VarBuilder, sorted by key
// like real files group them.
void flushVars(EntitySink &sink, VarBuilder &vars, quint16 high)
{
    std::stable_sort(sink.varBlobs.begin(), sink.varBlobs.end(),
                     [](const QPair<quint16, QByteArray> &a, const QPair<quint16, QByteArray> &b) {
                         return a.first < b.first;
                     });
    for (const auto &p : sink.varBlobs)
        vars.add(sink.uid, p.first, high, p.second);
    sink.varBlobs.clear();
}

// ---- notes / cost rates / calendar data blobs ---------------------------------

// Notes are the raw RTF source as an 8-bit NUL-terminated string (the reader's
// readNotesRtf mirror). Callers should pass ASCII-safe RTF.
QByteArray notesBlob(const QString &notes)
{
    QByteArray b = notes.toLatin1();
    b.append(char(0));
    return b;
}

double rateToHours(double rate, quint16 fmt)
{
    const int timeUnit = (fmt == 0xFFFF) ? 1 : (int(fmt) - 1);
    switch (timeUnit) {
    case 0:  return rate * 60.0;                       // per minute
    case 1:  return rate;                              // per hour
    case 2:  return rate / 8.0;                        // per day  (480 min/day)
    case 3:  return rate / 40.0;                       // per week (2400 min/week)
    case 5:  return rate * 60.0 / (2400.0 * 52.0);     // per year
    default: return rate;
    }
}

// Cost-rate table blob (MPXJ CostRateTableFactory layout): 16-byte header, then
// 44-byte entries. Open-ended entries store the "until further notice" end date.
QByteArray costRateBlob(const QList<schedule::CostRate> &entries)
{
    const QDateTime endNa(QDate(2049, 12, 31), QTime(23, 59), Qt::UTC);
    QByteArray b(16, '\0');
    for (const schedule::CostRate &e : entries) {
        QByteArray rec(44, '\0');
        const quint16 stdFmt = (e.standardRateUnit == 2) ? 0xFFFF : quint16(e.standardRateUnit);
        const quint16 otFmt = (e.overtimeRateUnit == 2) ? 0xFFFF : quint16(e.overtimeRateUnit);
        pokeDouble(rec, 0, rateToHours(e.standardRate, stdFmt));
        pokeU16(rec, 8, stdFmt);
        pokeDouble(rec, 16, rateToHours(e.overtimeRate, otFmt));
        pokeU16(rec, 24, otFmt);
        pokeDouble(rec, 32, e.costPerUse * 100.0);
        pokeU32(rec, 40, quint32(encodeTimestampTenths(e.endDate.isValid() ? e.endDate : endNa)));
        b += rec;
    }
    return b;
}

// Resource availability-table blob (MPXJ AvailabilityFactory layout, reverse-
// engineered from tests/fixtures/mpp14availability.mpp): 12-byte header ([u16
// segment count][10 reserved]), then (segments+1) 20-byte boundary slots
// [timestamp tenths @0, epoch 1983-12-31 -- NOT the same epoch
// encodeTimestampTenths uses][units double @4, ten-thousandths like MAX_UNITS]
// [8 unused]. Segment i spans [boundary[i], boundary[i+1] - 1 minute]; real
// periods are interleaved with zero-units filler segments for gaps/leading/
// trailing time, mirroring MS Project's own layout, so a reader (ours or
// MPXJ's) can recover independent, possibly non-adjacent periods by skipping
// zero-unit segments.
QByteArray availabilityBlob(const QList<schedule::AvailabilityPeriod> &periods)
{
    const QDateTime startNa(QDate(1983, 12, 31), QTime(0, 0), Qt::UTC);
    const QDateTime endNa(QDate(2049, 12, 31), QTime(23, 59), Qt::UTC);
    auto encodeTs = [&](const QDateTime &dt) -> quint32 {
        return quint32(static_cast<qint32>(startNa.secsTo(dt.toUTC()) / 6));
    };

    QList<schedule::AvailabilityPeriod> sorted = periods;
    std::sort(sorted.begin(), sorted.end(),
              [](const schedule::AvailabilityPeriod &a, const schedule::AvailabilityPeriod &b) {
                  if (a.startDate.isValid() != b.startDate.isValid())
                      return !a.startDate.isValid();   // open-start (invalid) sorts first
                  return a.startDate.isValid() && a.startDate < b.startDate;
              });

    struct Segment { QDateTime start; double units; };
    QList<Segment> segs;
    QDateTime cursor = startNa;
    for (const schedule::AvailabilityPeriod &p : sorted) {
        const QDateTime effStart = p.startDate.isValid() ? p.startDate : startNa;
        const QDateTime effEndExcl = (p.endDate.isValid() ? p.endDate : endNa).addSecs(60);
        if (effStart > cursor)
            segs.append({ cursor, 0.0 });         // gap filler
        segs.append({ effStart, p.units * 10000.0 });
        cursor = effEndExcl;
    }
    if (cursor < endNa.addSecs(60))
        segs.append({ cursor, 0.0 });              // trailing filler
    segs.append({ endNa.addSecs(60), 0.0 });        // terminating boundary (timestamp only)

    QByteArray b(12, '\0');
    pokeU16(b, 0, quint16(segs.size() - 1));   // count excludes the terminating boundary
    for (const Segment &s : segs) {
        QByteArray rec(20, '\0');
        pokeU32(rec, 0, encodeTs(s.start));
        pokeDouble(rec, 4, s.units);
        b += rec;
    }
    return b;
}

// The default working week the reader assumes for a "flag == 1" day of a base
// calendar (must match parseCalendarData's kMorning/kAfternoon).
bool isDefaultDay(bool isBase, int blobDayIndex, const QList<schedule::TimeRange> &ranges)
{
    if (!isBase)
        return ranges.isEmpty();   // derived calendars inherit
    const bool defWork = (blobDayIndex >= 1 && blobDayIndex <= 5);   // Mon..Fri
    if (!defWork)
        return ranges.isEmpty();
    return ranges.size() == 2
        && ranges[0] == schedule::TimeRange{ QTime(8, 0), QTime(12, 0) }
        && ranges[1] == schedule::TimeRange{ QTime(13, 0), QTime(17, 0) };
}

// A range's length in tenths of a minute. An end of midnight means "until the
// end of the day" (the 24 Hours calendar's 00:00-00:00, Night Shift's
// 23:00-00:00), mirroring WorkCalendar::toPeriods; a plain msecsTo() would go
// negative there and silently write a zero-length period.
int rangeDurationTenths(const schedule::TimeRange &r)
{
    constexpr int kMsPerDay = 24 * 60 * 60 * 1000;
    const int endMs = (r.end == QTime(0, 0)) ? kMsPerDay : r.end.msecsSinceStartOfDay();
    return qMax(0, endMs - r.start.msecsSinceStartOfDay()) / 6000;
}

// CALENDAR_DATA blob (var type 8): 7 x 60-byte day blocks (0=Sunday..6=Saturday),
// then the exceptions section at offset 420. Times are tenths of a minute.
QByteArray calendarDataBlob(const schedule::Calendar &c, bool isBase, bool *needed)
{
    static const int idxToDay[7] = { 6, 0, 1, 2, 3, 4, 5 };   // blob index -> model index
    const auto dayRanges = [&](int blobIdx) -> QList<schedule::TimeRange> {
        const int m = idxToDay[blobIdx];
        return (m < c.workingTimes.size()) ? c.workingTimes.at(m) : QList<schedule::TimeRange>();
    };

    *needed = !c.exceptions.isEmpty();
    QByteArray b(420, '\0');
    for (int i = 0; i < 7; ++i) {
        const QList<schedule::TimeRange> ranges = dayRanges(i);
        if (isDefaultDay(isBase, i, ranges)) {
            pokeU16(b, 60 * i, 1);
            continue;
        }
        *needed = true;
        pokeU16(b, 60 * i, 0);
        const int n = qMin(int(ranges.size()), 5);
        pokeU16(b, 60 * i + 2, quint16(n));
        for (int p = 0; p < n; ++p) {
            const int startTenths = ranges[p].start.msecsSinceStartOfDay() / 6000;
            pokeU16(b, 60 * i + 8 + p * 2, quint16(startTenths));
            pokeU16(b, 60 * i + 20 + p * 4, quint16(rangeDurationTenths(ranges[p])));
        }
    }

    if (!c.exceptions.isEmpty()) {
        const QDate epochDay(1983, 12, 31);
        appU16(b, quint16(c.exceptions.size()));
        appU16(b, 0);   // the reader (and MPXJ) skip 4 bytes past the count
        for (const schedule::CalendarException &ex : c.exceptions) {
            QByteArray blk(92, '\0');
            pokeU16(blk, 0, ex.fromDate.isValid() ? quint16(epochDay.daysTo(ex.fromDate)) : 0xFFFF);
            pokeU16(blk, 2, ex.toDate.isValid() ? quint16(epochDay.daysTo(ex.toDate)) : 0xFFFF);
            // Recurrence header (MPXJ readRecurringData): occurrences u16@4 and
            // type u16@72. Type 1 = daily with frequency forced to 1, which
            // readers treat as a plain one-off date-range exception.
            pokeU16(blk, 4, 1);
            pokeU16(blk, 72, 1);
            const int n = ex.working ? qMin(int(ex.workingTimes.size()), 5) : 0;
            pokeU16(blk, 14, quint16(n));
            for (int p = 0; p < n; ++p) {
                pokeU16(blk, 20 + p * 2,
                        quint16(ex.workingTimes[p].start.msecsSinceStartOfDay() / 6000));
                pokeU16(blk, 32 + p * 4, quint16(rangeDurationTenths(ex.workingTimes[p])));
            }
            quint32 nameLen = 0;
            QByteArray nameBytes;
            if (!ex.name.isEmpty()) {
                nameBytes = utf16zBytes(ex.name);
                while (nameBytes.size() % 4 != 0)
                    nameBytes.append(char(0));
                nameLen = quint32(nameBytes.size());
            }
            pokeU32(blk, 88, nameLen);
            b += blk;
            b += nameBytes;
        }
    }
    return b;
}

// ---- \005SummaryInformation ([MS-OLEPS]) --------------------------------------

QByteArray summaryInformationStream(const QString &title, const QString &author)
{
    struct Prop { quint32 id; QByteArray value; };
    QList<Prop> props;

    auto i2Value = [](quint16 v) {
        QByteArray b;
        appU32(b, 2);      // VT_I2
        appU32(b, v);      // stored padded to 4 bytes
        return b;
    };
    auto lpwstrValue = [](const QString &s) {
        QByteArray b;
        appU32(b, 0x1F);   // VT_LPWSTR
        const QByteArray chars = utf16zBytes(s);
        appU32(b, quint32(chars.size() / 2));   // char count, including the NUL
        b += chars;
        while (b.size() % 4 != 0)
            b.append(char(0));
        return b;
    };

    props.append({ 1, i2Value(1252) });                                  // codepage
    if (!title.isEmpty())
        props.append({ 2, lpwstrValue(title) });                         // PIDSI_TITLE
    if (!author.isEmpty())
        props.append({ 4, lpwstrValue(author) });                        // PIDSI_AUTHOR
    props.append({ 0x12, lpwstrValue(QStringLiteral("Microsoft Project")) }); // appname

    // Section: [size][count] + (id, offset) pairs + values.
    QByteArray values;
    QList<QPair<quint32, quint32>> pairs;
    const int valueBase = 8 + props.size() * 8;
    for (const Prop &p : props) {
        pairs.append({ p.id, quint32(valueBase + values.size()) });
        values += p.value;
    }
    QByteArray section;
    appU32(section, quint32(valueBase + values.size()));
    appU32(section, quint32(props.size()));
    for (const auto &pr : pairs) {
        appU32(section, pr.first);
        appU32(section, pr.second);
    }
    section += values;

    // Header: byte order, version, system id, zero CLSID, one property set with
    // FMTID_SummaryInformation at offset 48. Constants match real files.
    QByteArray s;
    appU16(s, 0xFFFE);
    appU16(s, 0);
    appU32(s, 0x00020A0A);
    s += QByteArray(16, '\0');
    appU32(s, 1);
    static const quint8 fmtid[16] = { 0xE0, 0x85, 0x9F, 0xF2, 0xF9, 0x4F, 0x68, 0x10,
                                      0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9 };
    s += QByteArray(reinterpret_cast<const char *>(fmtid), 16);
    appU32(s, 48);
    s += section;
    return s;
}

// ---- template plumbing ---------------------------------------------------------

// Recursively mirror the template tree into the output, skipping the streams a
// callback claims (regenerated storages) and preserving empty storages.
void copyTree(const CompoundFile &tpl, CompoundFile &out, const QStringList &path,
              const std::function<bool(const QStringList &)> &skipStream)
{
    for (const QString &name : tpl.childNames(path)) {
        QStringList child = path;
        child << name;
        if (tpl.hasStorage(child)) {
            out.addStorage(child);
            copyTree(tpl, out, child, skipStream);
        } else if (!skipStream(child)) {
            out.addStream(child, tpl.readStream(child));
        }
    }
}

// Patch a 4-byte Props item value in place (same walk as PropsReader::parse),
// leaving every other byte of the stream untouched.
bool patchPropsU32(QByteArray &props, quint32 key, quint32 value)
{
    int o = 16;
    while (o + 12 <= props.size()) {
        quint32 len = 0, k = 0;
        readU32(props, o, &len);
        readU32(props, o + 4, &k);
        o += 12;
        if (len > quint32(props.size() - o))
            return false;
        if (k == key && len >= 4)
            return pokeU32(props, o, value);
        o += int(len);
    }
    return false;
}

// Patch a UTF-16LE (null-terminated) Props string item, matching
// PropsReader::string()'s decode. Same-length replacements patch in place;
// a different length (or a key the template lacks) rebuilds the item and
// fixes up the two header "byteSize" fields by the size delta -- everything
// else in the stream (other items, the field maps) is untouched.
void patchPropsString(QByteArray &props, quint32 key, const QString &value)
{
    const QByteArray newData = utf16zBytes(value);
    int o = 16;
    while (o + 12 <= props.size()) {
        const int itemStart = o;
        quint32 len = 0, k = 0, flags = 0;
        readU32(props, o, &len);
        readU32(props, o + 4, &k);
        readU32(props, o + 8, &flags);
        o += 12;
        if (len > quint32(props.size() - o))
            break;
        if (k == key) {
            if (int(len) == newData.size()) {
                memcpy(props.data() + o, newData.constData(), size_t(newData.size()));
                return;
            }
            QByteArray newItem;
            appU32(newItem, quint32(newData.size()));
            appU32(newItem, key);
            appU32(newItem, flags);
            newItem += newData;
            const int oldItemLen = 12 + int(len);
            props.replace(itemStart, oldItemLen, newItem);
            const int delta = newItem.size() - oldItemLen;
            quint32 sz0 = 0, sz1 = 0;
            readU32(props, 0, &sz0);
            readU32(props, 4, &sz1);
            pokeU32(props, 0, quint32(int(sz0) + delta));
            pokeU32(props, 4, quint32(int(sz1) + delta));
            return;
        }
        o += int(len);
    }
    // Key absent (the template should always have it, but handle it anyway):
    // append a new item and grow both header size fields to match.
    QByteArray newItem;
    appU32(newItem, quint32(newData.size()));
    appU32(newItem, key);
    appU32(newItem, 0u);
    newItem += newData;
    props += newItem;
    quint32 sz0 = 0, sz1 = 0;
    readU32(props, 0, &sz0);
    readU32(props, 4, &sz1);
    pokeU32(props, 0, quint32(int(sz0) + newItem.size()));
    pokeU32(props, 4, quint32(int(sz1) + newItem.size()));
}

// Slice `count` meta items (without the 16-byte header) out of a meta stream.
QByteArray metaItems(const QByteArray &meta, int itemSize, int first, int count)
{
    return meta.mid(16 + first * itemSize, count * itemSize);
}

} // namespace

// -----------------------------------------------------------------------------

bool writeMpp14(const schedule::Project &in, CompoundFile &cf, QString *error)
{
    scheduleioEnsureResources();

    // Diagnostic hook: SCHEDULEIO_MPP14_TEMPLATE overrides the embedded
    // container template with any real .mpp, to isolate whether a rendering
    // difference comes from the regenerated streams or the template.
    QString tplPath = QStringLiteral(":/scheduleio/mpp14template.mpp");
    const QByteArray tplOverride = qgetenv("SCHEDULEIO_MPP14_TEMPLATE");
    if (!tplOverride.isEmpty())
        tplPath = QString::fromLocal8Bit(tplOverride);
    QFile tf(tplPath);
    if (!tf.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("MPP14 template missing: %1").arg(tplPath);
        return false;
    }
    CompoundFile tpl;
    if (!tpl.openFromData(tf.readAll())) {
        if (error) *error = QStringLiteral("MPP14 template is not a valid docfile: %1").arg(tpl.errorString());
        return false;
    }

    // The five storages we regenerate; everything else copies verbatim (their
    // per-entity Props streams included -- only the six quartet streams go).
    const QSet<QString> regen = { QStringLiteral("TBkndTask"), QStringLiteral("TBkndRsc"),
                                  QStringLiteral("TBkndAssn"), QStringLiteral("TBkndCons"),
                                  QStringLiteral("TBkndCal") };
    const QSet<QString> quartet = { QStringLiteral("FixedMeta"), QStringLiteral("FixedData"),
                                    QStringLiteral("Fixed2Meta"), QStringLiteral("Fixed2Data"),
                                    QStringLiteral("VarMeta"), QStringLiteral("Var2Data") };
    const QString summaryName = QString(QChar(0x05)) + QStringLiteral("SummaryInformation");

    // Gantt-view formatting (per-task fonts/colours, view style template) is
    // patched into the template's CV_iew var data; skip those two streams in
    // the verbatim copy only when there is something to patch.
    const QString viewStorage = QStringLiteral("   214");
    QByteArray fontProps = tpl.readStream({ viewStorage, QStringLiteral("Props") });
    QByteArray templateFontBases;
    for (int o = 16; o + 12 <= fontProps.size(); ) {
        const quint32 len = readLeU32(fontProps, o);
        const quint32 key = readLeU32(fontProps, o + 4);
        o += 12;
        if (len > quint32(fontProps.size() - o))
            break;
        if (key == 0x03400000u) {
            templateFontBases = fontProps.mid(o, int(len));
            break;
        }
        o += int(len);
        if (len % 2 != 0)
            ++o;
    }
    schedule::Project viewProject = in;
    ViewFormat::prepareFontBases(&viewProject, templateFontBases);
    const bool patchViews = ViewFormat::wantsPatch(viewProject);

    copyTree(tpl, cf, {}, [&](const QStringList &p) {
        if (p.size() == 3 && p.at(0) == kDataStorage && regen.contains(p.at(1))
            && quartet.contains(p.at(2)))
            return true;
        if (patchViews && p.size() == 3 && p.at(0) == viewStorage
            && p.at(1) == QStringLiteral("CV_iew")
            && (p.at(2) == QStringLiteral("VarMeta") || p.at(2) == QStringLiteral("Var2Data")))
            return true;   // regenerated by ViewFormat::patch below
        if (p.size() == 2 && p.at(0) == kDataStorage && p.at(1) == QStringLiteral("Props"))
            return true;   // patched below
        if (p.size() == 1 && p.at(0) == summaryName)
            return true;   // regenerated below
        return false;
    });

    // The view STYLE_DATA and COLUMN_PROPERTIES records refer to this table by
    // byte-sized index. The embedded template has a different index layout
    // from many real Project files, so retain the source payload whenever its
    // fixed-size table matches the template item.
    if (!viewProject.mppFontBases.isEmpty()) {
        for (int o = 16; o + 12 <= fontProps.size(); ) {
            const quint32 len = readLeU32(fontProps, o);
            const quint32 key = readLeU32(fontProps, o + 4);
            o += 12;
            if (len > quint32(fontProps.size() - o))
                break;
            if (key == 0x03400000u && len == quint32(viewProject.mppFontBases.size())) {
                pokeBytes(fontProps, o, viewProject.mppFontBases);
                cf.addStream({ viewStorage, QStringLiteral("Props") }, fontProps);
                break;
            }
            o += int(len);
            if (len % 2 != 0)
                ++o;
        }
    }

    if (patchViews && !ViewFormat::patch(tpl, cf, viewProject)) {
        // No usable Gantt view in the template: fall back to the verbatim copy.
        cf.addStream({ viewStorage, QStringLiteral("CV_iew"), QStringLiteral("VarMeta") },
                     tpl.readStream({ viewStorage, QStringLiteral("CV_iew"), QStringLiteral("VarMeta") }));
        cf.addStream({ viewStorage, QStringLiteral("CV_iew"), QStringLiteral("Var2Data") },
                     tpl.readStream({ viewStorage, QStringLiteral("CV_iew"), QStringLiteral("Var2Data") }));
    }

    // ---- project-level Props (dates) + SummaryInformation (title/author) ------
    QByteArray props = tpl.readStream({ kDataStorage, QStringLiteral("Props") });
    patchPropsU32(props, 0x02400002u, encodeMppTimestamp(in.startDate));    // PROJECT_START_DATE
    patchPropsU32(props, 0x02400003u, encodeMppTimestamp(in.finishDate));   // PROJECT_FINISH_DATE
    patchPropsU32(props, 0x02400045u, encodeMppTimestamp(in.statusDate));   // STATUS_DATE
    {
        // Project default calendar (PropsKey DEFAULT_CALENDAR_NAME = 37748750) is
        // stored by NAME; -1/not-found means the implicit "Standard" calendar.
        QString defaultCalName = QStringLiteral("Standard");
        for (const schedule::Calendar &c : in.calendars)
            if (c.uniqueId == in.calendarUniqueId && !c.name.isEmpty()) { defaultCalName = c.name; break; }
        patchPropsString(props, 37748750u, defaultCalName);
    }
    // Project title (PropsKey 0x02400008). MS Project reads the title from
    // here, NOT from \005SummaryInformation -- leaving it unpatched made every
    // written file open as "blank_template" (the template's stale value).
    patchPropsString(props, 0x02400008u, in.title);
    cf.addStream({ kDataStorage, QStringLiteral("Props") }, props);
    cf.addStream({ summaryName }, summaryInformationStream(in.title, in.author));

    // ---- field maps from the template (authentic Project 2016 maps) -----------
    PropsReader pr;
    pr.parse(props);
    const QByteArray taskFm = FieldMap::fieldMapBytes(pr, 0x00020014u, 0x03000014u);
    const QByteArray rscFm = FieldMap::fieldMapBytes(pr, 0x00020015u, 0x03000015u);
    const QByteArray assnFm = FieldMap::fieldMapBytes(pr, 0x00020017u, 0x03000017u);
    if (taskFm.isEmpty() || rscFm.isEmpty() || assnFm.isEmpty()) {
        if (error) *error = QStringLiteral("template is missing its entity field maps");
        return false;
    }
    const QHash<quint16, EntityFieldLoc> taskLoc =
        FieldMap::entityFieldLocations(taskFm, MppFieldIds::kTaskHigh);
    const QHash<quint16, EntityFieldLoc> rscLoc =
        FieldMap::entityFieldLocations(rscFm, MppFieldIds::kResourceHigh);
    const QHash<quint16, EntityFieldLoc> assnLoc =
        FieldMap::entityFieldLocations(assnFm, MppFieldIds::kAssignmentHigh);

    // ---- template record/meta byte templates -----------------------------------
    const auto tplStream = [&](const char *ent, const char *st) {
        return tpl.readStream({ kDataStorage, QLatin1String(ent), QLatin1String(st) });
    };
    const QByteArray tTaskFM = tplStream("TBkndTask", "FixedMeta");
    const QByteArray tTaskFD = tplStream("TBkndTask", "FixedData");
    const QByteArray tTaskF2M = tplStream("TBkndTask", "Fixed2Meta");
    const QByteArray tTaskF2D = tplStream("TBkndTask", "Fixed2Data");
    const QByteArray tRscFM = tplStream("TBkndRsc", "FixedMeta");
    const QByteArray tRscFD = tplStream("TBkndRsc", "FixedData");
    const QByteArray tRscF2M = tplStream("TBkndRsc", "Fixed2Meta");
    const QByteArray tRscF2D = tplStream("TBkndRsc", "Fixed2Data");
    const QByteArray tCalFM = tplStream("TBkndCal", "FixedMeta");
    const QByteArray tCalFD = tplStream("TBkndCal", "FixedData");
    const QByteArray tCalF2M = tplStream("TBkndCal", "Fixed2Meta");

    // Real-record templates: the template's own project-summary task row and
    // resource stub row (encode "everything absent/default" authentically).
    const QByteArray taskRecTpl = tTaskFD.mid(48, kTaskRecSize);
    const QByteArray taskMetaTailTpl = tTaskFM.mid(16 + 3 * kTaskMetaItem + 8, kTaskMetaItem - 8);
    const QByteArray taskF2Tpl = tTaskF2D.mid(192, kTaskF2Block);
    const QByteArray taskF2TailTpl = tTaskF2M.mid(16 + 3 * kTaskF2MetaItem + 8, kTaskF2MetaItem - 8);
    const QByteArray rscRecTpl = tRscFD.mid(48, kRscRecSize);
    if (taskRecTpl.size() != kTaskRecSize || rscRecTpl.size() != kRscRecSize) {
        if (error) *error = QStringLiteral("unexpected MPP14 template record geometry");
        return false;
    }

    const auto writeQuartet = [&](const char *ent, const FixedBuilder &fixed,
                                  const FixedBuilder &fixed2, const VarBuilder &vars) {
        const QString e = QLatin1String(ent);
        cf.addStream({ kDataStorage, e, QStringLiteral("FixedMeta") }, fixed.meta());
        cf.addStream({ kDataStorage, e, QStringLiteral("FixedData") }, fixed.data());
        cf.addStream({ kDataStorage, e, QStringLiteral("Fixed2Meta") }, fixed2.meta());
        cf.addStream({ kDataStorage, e, QStringLiteral("Fixed2Data") }, fixed2.data());
        cf.addStream({ kDataStorage, e, QStringLiteral("VarMeta") }, vars.meta());
        cf.addStream({ kDataStorage, e, QStringLiteral("Var2Data") }, vars.data());
    };

    // ======================= TBkndTask =========================================
    {
        FixedBuilder fixed(kTaskMetaItem), fixed2(kTaskF2MetaItem);
        VarBuilder vars;
        // The three 16-byte placeholder rows real files start with, verbatim.
        fixed.addRaw(metaItems(tTaskFM, kTaskMetaItem, 0, 3), tTaskFD.left(48));
        fixed2.addRaw(metaItems(tTaskF2M, kTaskF2MetaItem, 0, 3), tTaskF2D.left(192));

        // Emit task rows in UNIQUE-ID order. FixedData and VarMeta are correlated
        // in that order by Microsoft Project; display order is carried separately
        // in Fixed2Data (see the ID+1 value written below).
        QVector<const schedule::Task *> tasksByUid;
        tasksByUid.reserve(in.tasks.size());
        for (const schedule::Task &t : in.tasks)
            tasksByUid.append(&t);
        std::sort(tasksByUid.begin(), tasksByUid.end(),
                  [](const schedule::Task *a, const schedule::Task *b) {
                      return a->uniqueId < b->uniqueId;
                  });

        // Parent task per row (field 160, u32 at fixed offset 142): the
        // parent row's UNIQUE ID, where the parent of a row is the nearest
        // shallower row above it in DISPLAY order -- i.e. ordered by the ID
        // field (the row number), which in.tasks does not guarantee for
        // files with reordered unique ids (verified against Average
        // Project.mpp). The UID-0 project summary has none (-1). MS Project
        // uses this (not just the SUMMARY flag) to categorise rows: the
        // template stub's -1 on every row made every row render as a bold
        // project summary, and an inconsistent parent graph crashes Project
        // on open.
        QHash<int, quint32> parentUidOf;   // task uid -> parent's unique id
        {
            QVector<const schedule::Task *> byRow;
            byRow.reserve(in.tasks.size());
            for (const schedule::Task &t : in.tasks)
                byRow.append(&t);
            std::sort(byRow.begin(), byRow.end(),
                      [](const schedule::Task *a, const schedule::Task *b) {
                          return a->id < b->id;
                      });
            QVector<int> lastAtLevel(34, -1);
            for (const schedule::Task *t : byRow) {
                const int lvl = qBound(0, t->outlineLevel, 31);
                int parent = -1;
                for (int l = lvl - 1; l >= 0; --l)
                    if (lastAtLevel[l] >= 0) { parent = lastAtLevel[l]; break; }
                parentUidOf.insert(t->uniqueId, quint32(parent));
                lastAtLevel[lvl] = t->uniqueId;
                for (int l = lvl + 1; l < lastAtLevel.size(); ++l)
                    lastAtLevel[l] = -1;
            }
        }

        for (const schedule::Task *taskPtr : tasksByUid) {
            const schedule::Task &t = *taskPtr;
            const quint16 percentComplete = encodePercent(t.percentComplete);

            // Percent complete is the value users edit in Schedule Vault and
            // is the authoritative progress scalar in this model.  Project
            // also stores actual/remaining values and reconciles conflicting
            // copies when opening a file.  In particular, a stale full Actual
            // Duration makes Project change a task to 100%, while a stale
            // Actual Start pins an otherwise automatic task so predecessors no
            // longer move it.  The zero remaining-duration copy also makes a
            // leaf row acquire summary/roll-up-like rendering.
            //
            // Preserve genuine actuals (their exact rolled-up values can differ
            // slightly from percent * duration), but replace the complete set
            // when it represents a different integer completion percentage.
            const qint64 completedDuration = amountAtPercent(t.durationMillis, percentComplete);
            const qint64 remainingDuration = qMax<qint64>(0, t.durationMillis - completedDuration);
            const qint64 completedWork = amountAtPercent(t.workMillis, percentComplete);
            bool progressContradictsPercent = false;
            if (t.actualDurationMillis != 0) {
                if (t.durationMillis <= 0) {
                    progressContradictsPercent = true;
                } else {
                    const int actualDurationPercent = qBound(
                        0, int(qRound64(double(t.actualDurationMillis) * 100.0
                                        / double(t.durationMillis))), 100);
                    progressContradictsPercent |= actualDurationPercent != percentComplete;
                }
            }
            QDateTime actualStart = t.actualStart;
            QDateTime actualFinish = t.actualFinish;
            qint64 actualDuration = t.actualDurationMillis;
            qint64 actualWork = t.actualWorkMillis;
            if (progressContradictsPercent) {
                actualStart = percentComplete > 0
                    ? (t.actualStart.isValid() ? t.actualStart : t.start) : QDateTime();
                actualFinish = percentComplete == 100
                    ? (t.actualFinish.isValid() ? t.actualFinish : t.finish) : QDateTime();
                actualDuration = completedDuration;
                actualWork = completedWork;
            }

            QByteArray rec = taskRecTpl;
            QByteArray f2 = taskF2Tpl;
            EntitySink sink;
            sink.loc = &taskLoc;
            sink.b0 = &rec;
            sink.b1 = &f2;
            sink.uid = quint32(t.uniqueId);

            sink.putU32(86, quint32(t.uniqueId));                    // UNIQUE_ID
            sink.putU32(23, quint32(t.id));                          // ID
            sink.putDuration(29, t.durationMillis);                  // DURATION
            // Field 31 (u32@88): REMAINING duration (equals the full duration
            // on 0%-complete tasks, 0 on completed ones -- verified against
            // Average Project.mpp). Leaving it 0 made MS Project categorise
            // every row as a project-summary and render ALL task text in the
            // bold Calibri-12 ProjectSummary text style (proved by
            // byte-bisecting 03_hierarchy_dependencies).
            sink.putDuration(31, remainingDuration);
            sink.putDate(35, t.start);                               // (scheduled) START
            sink.putDate(36, t.finish);                              // (scheduled) FINISH
            // Project-authored completed tasks retain their calculated date
            // quartet. If these remain at the project-summary template's
            // sentinel, Project cannot recompute a completed auto task and
            // collapses its finish/duration when reopening the file.
            sink.putDate(37, t.start);                               // EARLY_START
            sink.putDate(38, t.finish);                              // EARLY_FINISH
            sink.putDate(39, t.lateStart.isValid() ? t.lateStart : t.start);   // LATE_START
            sink.putDate(40, t.lateFinish.isValid() ? t.lateFinish : t.finish); // LATE_FINISH
            sink.putU16(32, percentComplete);                        // PERCENT_COMPLETE
            // Keep the work-progress scalar coherent with task progress. The
            // current model has one progress value; emitting zero here for a
            // completed task makes Project recalculate its duration to zero.
            sink.putU16(33, percentComplete);                        // PERCENT_WORK_COMPLETE
            sink.putU16(249, quint16(t.outlineLevel));               // OUTLINE_LEVEL
            // Row-category fields (decoded from summary-vs-leaf diffs of the
            // mpp_samples ground truth; without them every row inherited the
            // template's project-summary values and MS Project rendered ALL
            // task text with the bold Summary text style):
            sink.putU16(128, t.summary ? 1 : 0);                     // SUMMARY
            sink.putU32(160, parentUidOf.value(t.uniqueId, quint32(-1)));  // PARENT_TASK_UID
            // Field 181 (u16@164): scheduling/state flags -- 0x35 on manual
            // rows, 0x15 for summaries, 0x07 for leaves (matches samples
            // 01/03/06/09 exactly; Average Project also shows 0x09 on some
            // leaves, meaning of that bit still unidentified).
            sink.putU16(181, t.manual ? 0x35 : (t.summary ? 0x15 : 0x07));
            // Internal task-state word at fixed offset 168. Project-authored
            // ordinary task rows carry bit 0x2000 in addition to the scaffold
            // summary's 0x0080. Without it Project treats scheduled rows as
            // already materialized/custom: zero-duration milestones acquire
            // actual dates and linked successors stop responding to edits.
            sink.putU32(201, 0x00002080u);
            sink.putU16(17, quint16(t.constraintType));              // CONSTRAINT_TYPE
            sink.putDate(18, t.constraintDate);                      // CONSTRAINT_DATE
            // Block 1 carries the manually scheduled tuple. Writing scheduled
            // values here for an automatic task makes Project regard the bar
            // as manually/custom formatted; on a zero-duration task it also
            // creates actual dates and marks the milestone complete. Leave the
            // scaffold defaults intact for automatic rows.
            if (t.manual) {
                sink.putDate(kTaskStartManual, t.start);
                sink.putDate(kTaskFinishManual, t.finish);
                sink.putDuration(1288, t.durationMillis);
                sink.putU16(1289, quint16(t.durationFormat));
            } else {
                // Project-authored automatic rows retain only a cached start
                // (not a complete manual span), a zero manual duration, and
                // the automatic-duration unit marker 0x15. The project-summary
                // scaffold carries different sentinels; copying those to a
                // zero-duration leaf makes Project infer completed actuals.
                sink.putDate(kTaskStartManual, t.summary ? QDateTime() : t.start);
                sink.putDate(kTaskFinishManual, QDateTime());
                sink.putDuration(1288, 0);
                sink.putU16(1289, 0x15);
            }
            sink.putU16(MppFieldIds::taskInfo.priority, quint16(t.priority));
            sink.putU16(MppFieldIds::taskInfo.taskType, quint16(t.taskType));
            sink.putDate(MppFieldIds::taskInfo.deadline, t.deadline);
            sink.putWork(MppFieldIds::taskInfo.work, t.workMillis);
            // LEVELING_DELAY: the template's field map leaves it unassigned
            // (META, no offset), so the sink cannot place it; store the u32
            // tenth-minutes as a Var2Data blob keyed by the field id, which the
            // reader falls back to when the field map has no location.
            if (t.levelingDelayMillis != 0) {
                QByteArray lvl;
                appU32(lvl, quint32(FieldDecoders::encodeDurationTenthMinutes(
                                t.levelingDelayMillis)));
                sink.putVarBlob(MppFieldIds::taskInfo.levelingDelay, lvl);
            }
            sink.putDate(MppFieldIds::taskActual.start, actualStart);
            sink.putDate(MppFieldIds::taskActual.finish, actualFinish);
            sink.putDuration(MppFieldIds::taskActual.duration, actualDuration);
            sink.putWork(MppFieldIds::taskActual.work, actualWork);
            sink.putDouble(MppFieldIds::taskCost.cost, t.cost);
            sink.putDouble(MppFieldIds::taskCost.fixedCost, t.fixedCost);
            sink.putDouble(MppFieldIds::taskCost.actualCost, t.actualCost);
            sink.putDouble(MppFieldIds::taskCost.remainingCost, t.remainingCost);
            sink.putDouble(MppFieldIds::taskCost.costVariance, t.costVariance);
            sink.putDouble(MppFieldIds::taskEvm.bcwp, t.evm.ev);
            sink.putDouble(MppFieldIds::taskEvm.bcws, t.evm.pv);
            sink.putDouble(MppFieldIds::taskEvm.acwp, t.evm.ac);
            sink.putDouble(MppFieldIds::taskEvm.cv, t.evm.cv);
            sink.putDouble(MppFieldIds::taskEvm.sv, t.evm.sv);
            sink.putDouble(MppFieldIds::taskEvm.cpi, t.evm.cpi);
            sink.putDouble(MppFieldIds::taskEvm.spi, t.evm.spi);
            sink.putDouble(MppFieldIds::taskEvm.eac, t.evm.eac);
            sink.putDouble(MppFieldIds::taskEvm.tcpi, t.evm.tcpi);
            putBaselines(sink, t.baselines, MppFieldIds::taskBaselines);
            putCustomFields(sink, t.customFields, MppFieldIds::kTaskHigh,
                            MppFieldIds::taskCustomFields());

            // NAME drives the reader's task discovery; write it first-class.
            if (!t.name.isEmpty())
                sink.putVarBlob(kTaskName, utf16zBytes(t.name));
            if (!t.notes.isEmpty())
                sink.putVarBlob(kTaskNotes, notesBlob(t.notes));
            flushVars(sink, vars, MppFieldIds::kTaskHigh);

            // Task GUID heads the Fixed2Data block.
            pokeBytes(f2, 0, guidFor("task", t.uniqueId));
            // Genuine Project 2016 rows store the display-order ordinal as a
            // double at Fixed2Data+16. It is one-based relative to the task ID
            // (project summary ID 0 -> 1, first visible task ID 1 -> 2).
            // Leaving the template's zero here made Project ignore field 23 and
            // rebuild the outline in unique-ID order.
            pokeDouble(f2, 16, double(t.id + 1));

            // FixedMeta bit flags: MILESTONE int@10 & 0x02, EFFORT_DRIVEN int@13
            // & 0x08 (Project 2013/2016 tables). The tail starts at item byte 8.
            QByteArray metaTail = taskMetaTailTpl;
            // The only full task row in the embedded scaffold is its project
            // summary. Three undocumented bits on that row are not harmless
            // defaults: copying them to ordinary rows makes Project classify
            // their bars as custom/summary-formatted and can turn an unstarted
            // zero-duration milestone into a completed task. Clear the two
            // summary-only bits and use the normal automatic-task value seen
            // on Project-authored MPP14 leaf and milestone rows.
            metaTail[1] = char(quint8(metaTail[1]) & ~0x40);
            metaTail[5] = char(quint8(metaTail[5]) & ~0x10);
            metaTail[9] = char(quint8(metaTail[9]) | 0x04);
            metaTail[2] = char(t.milestone ? (quint8(metaTail[2]) | 0x02)
                                           : (quint8(metaTail[2]) & ~0x02));
            metaTail[5] = char(t.effortDriven ? (quint8(metaTail[5]) | 0x08)
                                              : (quint8(metaTail[5]) & ~0x08));
            // Summary presence bit (tail byte 4 & 0x08): set on summary rows in
            // every genuine file, clear on leaves/milestones. The template tail
            // comes from the UID-0 project-summary stub, so it must be cleared
            // for non-summary rows or MS Project treats every row as a summary.
            metaTail[4] = char(t.summary ? (quint8(metaTail[4]) | 0x08)
                                         : (quint8(metaTail[4]) & ~0x08));
            // Notes presence: like the resource rows below, MS Project ignores
            // the notes Var2Data blob unless the row's FixedMeta advertises it
            // -- tail byte 36 bit 0x10 plus header bit 0x00010000 (both taken
            // from a task row MS Project itself wrote in 06_unicode_notes).
            quint32 metaHeader = 0x00080000u;
            if (!t.notes.isEmpty()) {
                metaTail[36] = char(quint8(metaTail[36]) | 0x10);
                metaHeader |= 0x00010000u;
            }
            fixed.addItem(metaHeader, rec, metaTail);

            // Fixed2Meta bit flags: TASK_MODE (manual) int@8 & 0x80.
            QByteArray f2Tail = taskF2TailTpl;
            f2Tail[0] = char(t.manual ? (quint8(f2Tail[0]) | 0x80)
                                      : (quint8(f2Tail[0]) & ~0x80));
            fixed2.addItem(0, f2, f2Tail);
        }
        writeQuartet("TBkndTask", fixed, fixed2, vars);
    }

    // ======================= TBkndRsc ==========================================
    {
        FixedBuilder fixed(kRscMetaItem), fixed2(kRscF2MetaItem);
        VarBuilder vars;
        // Placeholders + the uid-0 stub row real files carry, verbatim.
        fixed.addRaw(metaItems(tRscFM, kRscMetaItem, 0, 4), tRscFD.left(48 + kRscRecSize));
        fixed2.addRaw(metaItems(tRscF2M, kRscF2MetaItem, 0, 4), tRscF2D.left(4 * kRscF2Block));

        for (const schedule::Resource &r : in.resources) {
            QByteArray rec = rscRecTpl;
            EntitySink sink;
            sink.loc = &rscLoc;
            sink.b0 = &rec;
            sink.uid = quint32(r.uniqueId);

            sink.putU32(27, quint32(r.uniqueId));                    // UNIQUE_ID
            sink.putU32(0, quint32(r.id));                           // ID
            sink.putDouble(4, r.maxUnits * 10000.0, true);           // MAX_UNITS (ten-thousandths)
            sink.putDouble(MppFieldIds::resourceCost.cost, r.cost);
            sink.putDouble(MppFieldIds::resourceCost.actualCost, r.actualCost);
            sink.putDouble(MppFieldIds::resourceCost.remainingCost, r.remainingCost);
            sink.putDouble(MppFieldIds::resourceCost.costVariance, r.costVariance);
            putBaselines(sink, r.baselines, MppFieldIds::resourceBaselines);
            putCustomFields(sink, r.customFields, MppFieldIds::kResourceHigh,
                            MppFieldIds::resourceCustomFields());

            if (!r.name.isEmpty())
                sink.putVarBlob(kRscName, utf16zBytes(r.name));
            if (!r.initials.isEmpty())
                sink.putVarBlob(kRscInitials, utf16zBytes(r.initials));
            if (!r.notes.isEmpty())
                sink.putVarBlob(kRscNotes, notesBlob(r.notes));
            // Cost-rate tables A..E group by table into var keys 61..65.
            for (int table = 0; table < 5; ++table) {
                QList<schedule::CostRate> entries;
                for (const schedule::CostRate &cr : r.costRates)
                    if (cr.table == table)
                        entries.append(cr);
                if (!entries.isEmpty())
                    sink.putVarBlob(kCostRateVarKey[table], costRateBlob(entries));
            }
            if (!r.availabilityTable.isEmpty())
                sink.putVarBlob(kAvailabilityVarKey, availabilityBlob(r.availabilityTable));
            flushVars(sink, vars, MppFieldIds::kResourceHigh);

            // FixedMeta tail: live resource rows carry a field-presence bit
            // pattern the uid-0 stub lacks; with the stub's tail, real MS
            // Project shows every var-backed column (Name, Initials, ...)
            // blank even though the Var2Data blobs are present. Pattern from
            // Average Project.mpp live rows; notes rows additionally set
            // byte 3 bit 0x80 and byte 27 bit 0x40.
            QByteArray metaTail(kRscMetaItem - 8, '\0');
            static const quint8 kRscLiveTail[8] = { 0xE3, 0xFF, 0xFD, 0x3F,
                                                    0x5C, 0x46, 0x30, 0xC0 };
            memcpy(metaTail.data(), kRscLiveTail, 8);
            if (!r.notes.isEmpty()) {
                metaTail[3] = char(quint8(metaTail[3]) | 0x80);
                metaTail[27] = char(quint8(metaTail[27]) | 0x40);
            }
            fixed.addItem(0x00080000u, rec, metaTail);
            QByteArray f2(kRscF2Block, '\0');
            // RESOURCE_GUID (Fixed2 field 728). Assignment Fixed2 rows refer
            // back to this value; zeroing it leaves Project unable to bind the
            // assignment to its resource reliably.
            pokeBytes(f2, 0, guidFor("rsc", r.uniqueId));
            fixed2.addItem(0, f2);
        }
        writeQuartet("TBkndRsc", fixed, fixed2, vars);
    }

    // ======================= TBkndAssn =========================================
    {
        FixedBuilder fixed(kAssnMetaItem), fixed2(kAssnF2MetaItem);
        VarBuilder vars;
        QHash<int, const schedule::Task *> taskByUid;
        for (const schedule::Task &task : in.tasks)
            taskByUid.insert(task.uniqueId, &task);
        // Assignment metadata carries progress state separately from the work
        // fields. In particular, a zero-work milestone is distinguishable as
        // unstarted only through this metadata; using the completed-row pattern
        // makes Project synthesize task actuals and pin downstream links.
        static const quint8 assnUnstartedTail[8] =
            { 0xF1, 0xFF, 0x23, 0xF0, 0x69, 0x81, 0xC3, 0x03 };
        static const quint8 assnProgressTail[8] =
            { 0xF1, 0xFF, 0x3B, 0xF0, 0x69, 0x81, 0xC7, 0x03 };

        for (const schedule::Assignment &a : in.assignments) {
            QDateTime assignmentStart = a.start;
            QDateTime assignmentFinish = a.finish;
            const schedule::Task *task = taskByUid.value(a.taskUniqueId, nullptr);
            if (task && task->start.isValid() && task->finish.isValid()
                && assignmentStart.isValid() && assignmentFinish.isValid()
                && (assignmentStart < task->start || assignmentStart > task->finish
                    || assignmentFinish < task->start || assignmentFinish > task->finish
                    || assignmentFinish < assignmentStart)) {
                // A task may have been rescheduled after its assignments were
                // loaded. Project treats an assignment span outside its task
                // as authoritative progress and can turn the moved task into
                // a completed zero-day milestone. Keep valid delayed spans,
                // but re-anchor a wholly stale span to its task.
                assignmentStart = task->start;
                assignmentFinish = task->finish;
            }
            QByteArray rec(kAssnRecSize, '\0');
            EntitySink sink;
            sink.loc = &assnLoc;
            sink.b0 = &rec;
            sink.uid = quint32(a.uniqueId);

            sink.putU32(0, quint32(a.uniqueId));                     // UNIQUE_ID
            sink.putU32(1, quint32(a.taskUniqueId));                 // TASK_UNIQUE_ID
            sink.putU32(2, quint32(a.resourceUniqueId));             // RESOURCE_UNIQUE_ID
            sink.putDouble(7, a.units * 10000.0, true);   // UNITS (hundredths of a percent)
            // Work doubles are thousandths of a minute, like every work field
            // (ms / 60). An earlier build wrote tenths, 100x too small vs real
            // files; the reader's decodeWorkDouble is the inverse of this.
            sink.putDouble(8, double(a.workMillis) / 60.0, true);    // WORK
            sink.putDouble(10, double(a.actualWorkMillis) / 60.0, true);     // ACTUAL_WORK
            // With no overtime split in the model, all assignment work is
            // regular work. Project uses this together with the actual dates
            // to retain completed auto-task durations on reopen.
            sink.putDouble(11, double(a.workMillis) / 60.0, true);   // REGULAR_WORK
            sink.putDouble(12, double(a.remainingWorkMillis) / 60.0, true);  // REMAINING_WORK
            if (assignmentStart.isValid())
                sink.putU32(20, FieldDecoders::encodeMppTimestamp(assignmentStart), true);   // START
            if (assignmentFinish.isValid())
                sink.putU32(21, FieldDecoders::encodeMppTimestamp(assignmentFinish), true);  // FINISH
            if (a.actualWorkMillis > 0 && a.remainingWorkMillis == 0) {
                if (assignmentStart.isValid())
                    sink.putU32(22, FieldDecoders::encodeMppTimestamp(assignmentStart), true); // ACTUAL_START
                if (assignmentFinish.isValid()) {
                    const quint32 finish = FieldDecoders::encodeMppTimestamp(assignmentFinish);
                    sink.putU32(23, finish, true);                    // ACTUAL_FINISH
                    sink.putU32(24, finish, true);                    // RESUME
                    sink.putU32(264, finish, true);                   // STOP
                }
            } else if (assignmentStart.isValid()) {
                // Project-authored unstarted assignments carry Stop/Resume at
                // their scheduled start. Leaving both absent makes Project
                // synthesize actual dates; a zero-work milestone is then
                // silently changed to 100% complete and stops propagating links.
                const quint32 start = FieldDecoders::encodeMppTimestamp(assignmentStart);
                sink.putU32(24, start, true);                         // RESUME
                sink.putU32(264, start, true);                        // STOP
            }
            sink.putU32(25, quint32(FieldDecoders::encodeDurationTenthMinutes(a.delayMillis)),
                        true);                                       // DELAY
            sink.putU16(55, 7, true);                                // LEVELING_DELAY_UNITS (days)
            sink.putDouble(MppFieldIds::assignmentCost.cost, a.cost);
            sink.putDouble(MppFieldIds::assignmentCost.actualCost, a.actualCost);
            sink.putDouble(MppFieldIds::assignmentCost.remainingCost, a.remainingCost);
            sink.putDouble(MppFieldIds::assignmentCost.costVariance, a.costVariance);
            putBaselines(sink, a.baselines, MppFieldIds::assignmentBaselines);
            putCustomFields(sink, a.customFields, MppFieldIds::kAssignmentHigh,
                            MppFieldIds::assignmentCustomFields());
            if (!a.notes.isEmpty())
                sink.putVarBlob(kAssnNotes, notesBlob(a.notes));
            // CREATED (index 634): written for every assignment like real files.
            // MPXJ only accepts assignment rows whose unique id appears in the
            // VarMeta, so each assignment needs at least one var entry.
            {
                QByteArray created;
                appU32(created, encodeMppTimestamp(in.startDate));
                sink.putVarBlob(634, created);
            }
            flushVars(sink, vars, MppFieldIds::kAssignmentHigh);

            const bool unstarted = a.actualWorkMillis == 0
                && (!task || task->percentComplete <= 0.0);
            const quint8 *tailBytes = unstarted ? assnUnstartedTail : assnProgressTail;
            const QByteArray metaTail(reinterpret_cast<const char *>(tailBytes), 8);
            fixed.addItem(0x000C0000u, rec, metaTail);
            // Assignment Fixed2Data is three GUIDs: assignment, task and
            // resource. Project uses these links in addition to the integer
            // UIDs. Missing links can make zero-work milestones appear
            // completed and prevent dependency recalculation after editing.
            QByteArray f2 = guidFor("assn", a.uniqueId)
                + guidFor("task", a.taskUniqueId)
                + (a.resourceUniqueId < 0
                       ? kUnassignedResourceGuid
                       : guidFor("rsc", a.resourceUniqueId));
            QByteArray f2Tail(kAssnF2MetaItem - 8, '\0');
            // Field-presence bit carried by every Project-authored assignment
            // Fixed2 row examined (the GUID triplet is present).
            f2Tail[27] = char(0x10);
            fixed2.addItem(0, f2, f2Tail);
        }
        writeQuartet("TBkndAssn", fixed, fixed2, vars);
    }

    // ======================= TBkndCons (predecessor links) =====================
    {
        FixedBuilder fixed(kConsMetaItem), fixed2(kConsF2MetaItem);
        VarBuilder vars;
        QByteArray consMetaTail;
        appU16(consMetaTail, 0x00DD);   // constant per-item word observed in real files
        QByteArray consF2Tail;
        consF2Tail.append(char(0x07));

        // MSPDI carries no per-link UID, so every relation XmlIO parses has
        // uniqueId == 0. Synthesize a distinct id for those rows only, seeded
        // above the highest real relation id already present, so synthesized
        // ids can never collide with a genuine one from the binary reader or
        // the API.
        int nextSynthConsUid = 0;
        for (const schedule::Relation &r : in.relations)
            nextSynthConsUid = std::max(nextSynthConsUid, r.uniqueId);
        ++nextSynthConsUid;

        for (const schedule::Relation &r : in.relations) {
            // Never let a zero-uid or self-referencing relation reach the real
            // .mpp link table: MS Project treats predecessor/successor task uid
            // 0 (the Project Summary Task) as a circular reference and rejects
            // the file on next open. Mirrors docserializer.cpp's
            // readRealRelations guard so no upstream caller of this writer --
            // XmlIO, MppIO, or future readers -- can produce a file MS Project
            // won't reopen.
            if (r.predecessorTaskUid == 0 || r.successorTaskUid == 0
                    || r.predecessorTaskUid == r.successorTaskUid)
                continue;

            const int consUid = r.uniqueId > 0 ? r.uniqueId : nextSynthConsUid++;

            QByteArray rec(kConsRecSize, '\0');
            pokeU32(rec, 0, quint32(consUid));
            pokeU32(rec, 4, quint32(r.predecessorTaskUid));
            pokeU32(rec, 8, quint32(r.successorTaskUid));
            pokeU16(rec, 12, quint16(r.type));
            pokeU32(rec, 14, quint32(encodeDurationTenthMinutes(r.lagMillis)));
            pokeU16(rec, 18, quint16(r.lagFormat));   // lag display unit; time is @14
            fixed.addItem(0, rec, consMetaTail);

            // Fixed2Data: [relation GUID][predecessor task GUID][successor task GUID].
            QByteArray f2 = guidFor("cons", consUid)
                + guidFor("task", r.predecessorTaskUid)
                + guidFor("task", r.successorTaskUid);
            fixed2.addItem(0, f2, consF2Tail);
        }
        writeQuartet("TBkndCons", fixed, fixed2, vars);
    }

    // ======================= TBkndCal ==========================================
    {
        FixedBuilder fixed(kCalMetaItem), fixed2(kCalF2MetaItem);
        VarBuilder vars;
        // Four placeholder rows, verbatim from the template.
        fixed.addRaw(metaItems(tCalFM, kCalMetaItem, 0, 4), tCalFD.left(64));
        fixed2.addRaw(metaItems(tCalF2M, kCalF2MetaItem, 0, 4), QByteArray(4 * kCalF2Block, '\0'));

        // Resource calendars are linked back to their owning resource via
        // Resource::calendarUniqueId (authoritative); fall back to matching by
        // name (the calendar's name mirrors its resource's) for calendars that
        // predate that field, e.g. scaffold-path round trips.
        QHash<int, int> resUidByCalUid;
        QHash<QString, int> resUidByName;
        for (const schedule::Resource &r : in.resources) {
            if (r.calendarUniqueId >= 0)
                resUidByCalUid.insert(r.calendarUniqueId, r.uniqueId);
            if (!r.name.isEmpty() && !resUidByName.contains(r.name))
                resUidByName.insert(r.name, r.uniqueId);
        }

        for (const schedule::Calendar &c : in.calendars) {
            const bool isBase = (c.baseCalendarUniqueId < 0);
            // Base calendars other than Standard (uid 1) must use the pattern
            // real files use for user-created base calendars ("Copy of
            // Standard": base field 0, flags 0x00020000, meta tail 0x00CF).
            // The 0xFFFFFFFF/0x00010000/0x00AF shape is what MS Project writes
            // for its internal "Project 98 Baseline" calendar, and real MS
            // Project HIDES such rows from the project's base-calendar list.
            const bool isStandard = isBase && c.uniqueId == 1;
            QByteArray rec(kCalRecSize, '\0');
            pokeU32(rec, 0, isStandard ? 0xFFFFFFFFu
                                       : (isBase ? 0u : quint32(c.baseCalendarUniqueId)));
            const quint32 resId = resUidByCalUid.contains(c.uniqueId)
                                       ? quint32(resUidByCalUid.value(c.uniqueId))
                                       : resUidByName.value(c.name, 0);
            pokeU32(rec, 4, isBase ? 0xFFFFFFFFu : resId);
            pokeU32(rec, 8, quint32(c.uniqueId));

            QByteArray metaTail;
            appU16(metaTail, isStandard ? 0x008F : (isBase ? 0x00CF : 0x000E));
            fixed.addItem(isStandard ? 0x00010000u : (isBase ? 0x00020000u : 0u),
                          rec, metaTail);
            QByteArray f2Tail;
            appU16(f2Tail, isStandard ? 0x001E : 0x000E);
            fixed2.addItem(0, QByteArray(kCalF2Block, '\0'), f2Tail);

            if (isBase && !c.name.isEmpty())
                vars.add(quint32(c.uniqueId), kCalName, kCalHigh, utf16zBytes(c.name));
            bool needed = false;
            const QByteArray calData = calendarDataBlob(c, isBase, &needed);
            if (needed)
                vars.add(quint32(c.uniqueId), kCalData, kCalHigh, calData);
        }
        writeQuartet("TBkndCal", fixed, fixed2, vars);
    }

    // Project's backend opens each child rowset through duplicated bookkeeping
    // in the parent Props streams.  Reconcile this only after every 114 rowset
    // and any patched 214 view data have reached their final sizes.
    QString manifestError;
    if (!Mpp14Manifest::reconcile(cf, &manifestError)) {
        if (error)
            *error = manifestError;
        return false;
    }
    const QStringList manifestIssues = Mpp14Manifest::audit(cf);
    if (!manifestIssues.isEmpty()) {
        if (error)
            *error = QStringLiteral("inconsistent MPP14 rowset manifest: %1")
                         .arg(manifestIssues.join(QStringLiteral("; ")));
        return false;
    }

    return true;
}
