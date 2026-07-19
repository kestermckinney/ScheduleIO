// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/viewformat.h"

#include "codec/bkndvardata.h"
#include "model/project.h"
#include "ole/compoundfile.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace {

const QString kViewStorage = QStringLiteral("   214");
const QString kCView = QStringLiteral("CV_iew");

// Props9 item keys (MPXJ PropsKey).
constexpr quint32 kKeyStyleData = 574619656u;         // STYLE_DATA
constexpr quint32 kKeyColumnProperties = 574619660u;  // COLUMN_PROPERTIES
constexpr quint32 kKeyFontBases = 54525952u;          // 0x03400000, in 214/Props

constexpr quint16 kViewPropsType = 6;   // GanttChartView14.PROPERTIES
// MPP view-type codes (u16@112 of the FixedData record). These do NOT match
// MPXJ's ViewType enum ordinals -- verified against real files by view name.
constexpr quint16 kViewTypeGantt = 1;
constexpr quint16 kViewTypeResourceUsage = 15;
constexpr quint16 kViewTypeTeamPlanner = 9;
constexpr quint16 kViewTypeCalendar = 13;
constexpr int kViewRecordSize = 138;    // CV_iew FixedData block size

// Non-Gantt views store text styles in the same STYLE_DATA structure but their
// gridline/bar sections (and category count) differ and are undecoded, so only
// the text-style region is round-tripped. Cap the categories we touch to stay
// well inside every view's text region (never into its gridline section).
constexpr int kNonGanttTextCategories = 12;

// STYLE_DATA geometry (MPXJ GanttChartView14.processViewProperties).
constexpr int kTextStyleBase = 26;      // first default text style
constexpr int kTextStyleStride = 32;
constexpr int kGridSheetRows = 667;
constexpr int kGridSheetColumns = 697;
constexpr int kGridGanttRows = 847;
constexpr int kGridCurrentDate = 907;
constexpr int kGridStatusDate = 1027;
constexpr int kBarCountOffset = 2243;
constexpr int kBarStylesBase = 2255;
constexpr int kBarStyleSize = 195;
constexpr int kStyleDataMinSize = 2255; // enough for text styles + gridlines + bar header

// Per-cell/row exceptional-style records. Field ids are 0x0B400000 | MPXJ
// MPPTaskField FIELD_ARRAY index for a single cell; 0xFFFFFFFF means "the
// whole row". When the user formats an entire row, MS Project itself writes
// exactly two records: one for the ID column (field 23) and one whole-row
// record (verified against a file real Project wrote via SelectRow +
// Font32Ex).
constexpr quint16 kFieldName = 14;
constexpr quint16 kFieldId = 23;
constexpr quint32 kTaskFieldBase = 0x0B400000u;
constexpr quint32 kFieldWholeRow = 0xFFFFFFFFu;

// Change-mask bits at record offset 40 (decoded from the 01/10 fixture
// samples and the SelectRow ground truth): what the record actually applies.
constexpr quint16 kChgBold = 0x01, kChgUnderline = 0x02, kChgItalic = 0x04,
                  kChgColor = 0x08, kChgFontBase = 0x10, kChgBackColor = 0x40,
                  kChgPattern = 0x80;
constexpr quint16 kChgStrike = 0x100;
constexpr quint16 kChgAnyStyle = kChgBold | kChgUnderline | kChgItalic
                                 | kChgColor | kChgBackColor | kChgPattern | kChgStrike;

// ---- little-endian helpers ---------------------------------------------------

quint16 rdU16(const QByteArray &b, int o)
{
    if (o < 0 || o + 2 > b.size())
        return 0;
    const auto *p = reinterpret_cast<const uchar *>(b.constData() + o);
    return quint16(p[0] | (p[1] << 8));
}

quint32 rdU32(const QByteArray &b, int o)
{
    if (o < 0 || o + 4 > b.size())
        return 0;
    const auto *p = reinterpret_cast<const uchar *>(b.constData() + o);
    return quint32(p[0] | (p[1] << 8) | (p[2] << 16) | (quint32(p[3]) << 24));
}

void wrU16(QByteArray &b, int o, quint16 v)
{
    if (o < 0 || o + 2 > b.size())
        return;
    auto *p = reinterpret_cast<uchar *>(b.data() + o);
    p[0] = uchar(v & 0xFF);
    p[1] = uchar(v >> 8);
}

void wrU32(QByteArray &b, int o, quint32 v)
{
    if (o < 0 || o + 4 > b.size())
        return;
    auto *p = reinterpret_cast<uchar *>(b.data() + o);
    p[0] = uchar(v & 0xFF);
    p[1] = uchar((v >> 8) & 0xFF);
    p[2] = uchar((v >> 16) & 0xFF);
    p[3] = uchar(v >> 24);
}

void appU16(QByteArray &b, quint16 v) { int o = b.size(); b.resize(o + 2); wrU16(b, o, v); }
void appU32(QByteArray &b, quint32 v) { int o = b.size(); b.resize(o + 4); wrU32(b, o, v); }

// A colour cell: r,g,b + flag byte (0 = explicit, nonzero = "Automatic").
qint32 rdColor(const QByteArray &b, int o)
{
    if (o < 0 || o + 4 > b.size())
        return schedule::TextStyle::kAutomatic;
    const auto *p = reinterpret_cast<const uchar *>(b.constData() + o);
    if (p[3] != 0)
        return schedule::TextStyle::kAutomatic;
    return qint32((p[0] << 16) | (p[1] << 8) | p[2]);
}

void wrColor(QByteArray &b, int o, qint32 rgb)
{
    if (o < 0 || o + 4 > b.size())
        return;
    auto *p = reinterpret_cast<uchar *>(b.data() + o);
    if (rgb == schedule::TextStyle::kAutomatic) {
        p[0] = p[1] = p[2] = 0;
        p[3] = 0xFF;
    } else {
        p[0] = uchar((rgb >> 16) & 0xFF);
        p[1] = uchar((rgb >> 8) & 0xFF);
        p[2] = uchar(rgb & 0xFF);
        p[3] = 0;
    }
}

// UTF-16LE zero-terminated string at `o` (bounded by the record end).
QString rdUtf16(const QByteArray &b, int o, int maxBytes)
{
    QString s;
    for (int i = o; i + 1 < b.size() && i < o + maxBytes; i += 2) {
        const ushort ch = ushort(rdU16(b, i));
        if (ch == 0)
            break;
        s.append(QChar(ch));
    }
    return s;
}

// ---- CV_iew navigation ---------------------------------------------------------

// The id of the first non-split view of `viewType` in a CV_iew storage, or -1.
// Mirrors MPXJ MPP14Reader.processViewData: meta item u16@4 is the record's
// offset into the 138-byte-block FixedData; splitViewFlag u16@110 must be 0.
int findViewUid(const QByteArray &fixedMeta, const QByteArray &fixedData, quint16 viewType)
{
    if (fixedMeta.size() < 16)
        return -1;
    const int items = int(rdU32(fixedMeta, 8));
    int lastOffset = -1;
    for (int i = 0; i < items; ++i) {
        const int metaOff = 16 + i * 10;
        if (metaOff + 10 > fixedMeta.size())
            break;
        const int offset = rdU16(fixedMeta, metaOff + 4);
        if (offset <= lastOffset)
            continue;
        lastOffset = offset;
        if (offset + kViewRecordSize > fixedData.size())
            continue;
        const QByteArray rec = fixedData.mid(offset, kViewRecordSize);
        if (rdU16(rec, 110) == 1)
            continue;   // split view container, not a real view
        if (rdU16(rec, 112) == viewType)
            return int(rdU32(rec, 0));
    }
    return -1;
}

// ---- Props9 ---------------------------------------------------------------------

struct PropsItem {
    quint32 key = 0;
    quint32 flags = 0;
    QByteArray data;
};

struct Props9 {
    QByteArray header;   // 16 bytes, kept verbatim (item count patched on build)
    QVector<PropsItem> items;

    PropsItem *find(quint32 key)
    {
        for (PropsItem &it : items)
            if (it.key == key)
                return &it;
        return nullptr;
    }
    const PropsItem *find(quint32 key) const
    {
        for (const PropsItem &it : items)
            if (it.key == key)
                return &it;
        return nullptr;
    }
};

bool parseProps9(const QByteArray &block, Props9 *out)
{
    if (block.size() < 16)
        return false;
    out->header = block.left(16);
    out->items.clear();
    const int count = rdU16(block, 12);
    int o = 16;
    for (int i = 0; i < count; ++i) {
        if (o + 12 > block.size())
            break;
        const int size = int(rdU32(block, o));
        const quint32 key = rdU32(block, o + 4);
        const quint32 flags = rdU32(block, o + 8);
        o += 12;
        if (size < 1 || o + size > block.size())
            break;
        PropsItem item;
        item.key = key;
        item.flags = flags;
        item.data = block.mid(o, size);
        out->items.append(item);
        o += size;
        if (size % 2 != 0)
            ++o;   // items are 2-byte aligned
    }
    return !out->items.isEmpty();
}

QByteArray buildProps9(const Props9 &props)
{
    QByteArray b = props.header;
    wrU16(b, 12, quint16(props.items.size()));
    for (const PropsItem &it : props.items) {
        appU32(b, quint32(it.data.size()));
        appU32(b, it.key);
        appU32(b, it.flags);
        b += it.data;
        if (it.data.size() % 2 != 0)
            b.append('\0');
    }
    // Bytes 0-3 (duplicated at 4-7) are a self-describing "remaining size"
    // field (block size minus these first 4 bytes). Stale here (e.g. after
    // appending an item) causes MS Project's own parser to misread/reject
    // the block when constructing the view -- verified against a block MS
    // Project itself wrote after growing via Format>Font.
    const quint32 remaining = quint32(b.size() - 4);
    wrU32(b, 0, remaining);
    wrU32(b, 4, remaining);
    return b;
}

// ---- STYLE_DATA <-> ViewStyles ---------------------------------------------------

schedule::TextStyle readTextStyle(const QByteArray &d, int o)
{
    schedule::TextStyle s;
    s.fontBaseIndex = uchar(d.at(o));
    const int bits = uchar(d.at(o + 3));
    s.bold = (bits & 0x01) != 0;
    s.italic = (bits & 0x02) != 0;
    s.underline = (bits & 0x04) != 0;
    s.strikethrough = (bits & 0x08) != 0;
    s.color = rdColor(d, o + 4);
    s.backColor = rdColor(d, o + 16);
    s.backPattern = rdU16(d, o + 28);
    return s;
}

void writeTextStyle(QByteArray &d, int o, const schedule::TextStyle &s)
{
    if (s.fontBaseIndex >= 0 && s.fontBaseIndex <= 255)
        d[o] = char(s.fontBaseIndex);
    uchar bits = uchar(d.at(o + 3)) & ~0x0F;   // keep any unknown high bits
    bits |= (s.bold ? 0x01 : 0) | (s.italic ? 0x02 : 0)
        | (s.underline ? 0x04 : 0) | (s.strikethrough ? 0x08 : 0);
    d[o + 3] = char(bits);
    wrColor(d, o + 4, s.color);
    wrColor(d, o + 16, s.backColor);
    wrU16(d, o + 28, quint16(s.backPattern));
}

schedule::ViewLineStyle readGridLine(const QByteArray &d, int o)
{
    schedule::ViewLineStyle g;
    g.color = rdColor(d, o);
    g.lineStyle = uchar(d.at(o + 13));
    return g;
}

void writeGridLine(QByteArray &d, int o, const schedule::ViewLineStyle &g)
{
    wrColor(d, o, g.color);
    d[o + 13] = char(g.lineStyle);
}

// Map a default bar style's name onto the ViewStyles category it edits.
schedule::ViewBarStyle *barForName(schedule::ViewStyles &vs, const QString &name)
{
    if (name.compare(QLatin1String("Task"), Qt::CaseInsensitive) == 0)
        return &vs.taskBar;
    if (name.compare(QLatin1String("Milestone"), Qt::CaseInsensitive) == 0)
        return &vs.milestone;
    if (name.compare(QLatin1String("Summary"), Qt::CaseInsensitive) == 0)
        return &vs.summaryBar;
    if (name.compare(QLatin1String("Project Summary"), Qt::CaseInsensitive) == 0)
        return &vs.projectSummaryBar;
    return nullptr;
}

void readStyleData(const QByteArray &d, schedule::ViewStyles *vs)
{
    if (d.size() < kStyleDataMinSize)
        return;
    vs->present = true;
    for (int i = 0; i < schedule::ViewStyles::TextCategoryCount; ++i)
        vs->text[i] = readTextStyle(d, kTextStyleBase + i * kTextStyleStride);
    vs->sheetRows = readGridLine(d, kGridSheetRows);
    vs->sheetColumns = readGridLine(d, kGridSheetColumns);
    vs->ganttRows = readGridLine(d, kGridGanttRows);
    vs->currentDateLine = readGridLine(d, kGridCurrentDate);
    vs->statusDateLine = readGridLine(d, kGridStatusDate);

    const int barCount = uchar(d.at(kBarCountOffset));
    for (int i = 0; i < barCount; ++i) {
        const int o = kBarStylesBase + i * kBarStyleSize;
        if (o + kBarStyleSize > d.size())
            break;
        const QString name = rdUtf16(d, o + 91, kBarStyleSize - 91);
        if (schedule::ViewBarStyle *bar = barForName(*vs, name)) {
            bar->middleColor = rdColor(d, o + 2);
            bar->startColor = rdColor(d, o + 16);
            bar->endColor = rdColor(d, o + 29);
        }
    }
}

void patchStyleData(QByteArray &d, const schedule::ViewStyles &vs)
{
    if (d.size() < kStyleDataMinSize)
        return;
    for (int i = 0; i < schedule::ViewStyles::TextCategoryCount; ++i)
        writeTextStyle(d, kTextStyleBase + i * kTextStyleStride, vs.text[i]);
    writeGridLine(d, kGridSheetRows, vs.sheetRows);
    writeGridLine(d, kGridSheetColumns, vs.sheetColumns);
    writeGridLine(d, kGridGanttRows, vs.ganttRows);
    writeGridLine(d, kGridCurrentDate, vs.currentDateLine);
    writeGridLine(d, kGridStatusDate, vs.statusDateLine);

    const int barCount = uchar(d.at(kBarCountOffset));
    for (int i = 0; i < barCount; ++i) {
        const int o = kBarStylesBase + i * kBarStyleSize;
        if (o + kBarStyleSize > d.size())
            break;
        const QString name = rdUtf16(d, o + 91, kBarStyleSize - 91);
        const schedule::ViewBarStyle *bar = barForName(const_cast<schedule::ViewStyles &>(vs), name);
        if (bar) {
            wrColor(d, o + 2, bar->middleColor);
            wrColor(d, o + 16, bar->startColor);
            wrColor(d, o + 29, bar->endColor);
        }
    }
}

// Patch only the text-style region, capped at kNonGanttTextCategories so we
// never write past a smaller view's text region into its gridline section.
void patchViewTextStyles(QByteArray &d, const schedule::ViewStyles &vs)
{
    const int cats = qMin(int(schedule::ViewStyles::TextCategoryCount), kNonGanttTextCategories);
    if (d.size() < kTextStyleBase + cats * kTextStyleStride)
        return;
    for (int i = 0; i < cats; ++i)
        writeTextStyle(d, kTextStyleBase + i * kTextStyleStride, vs.text[i]);
}

// Patch the STYLE_DATA text-style bytes IN PLACE within a raw Props9 blob,
// leaving every other byte (all other items, header, padding) untouched, so a
// view whose full Props9 layout we don't reproduce still round-trips exactly.
// Returns the same-size blob, or an empty QByteArray if there's nothing to do.
QByteArray patchViewTextStylesInBlob(const QByteArray &blob, const schedule::ViewStyles &vs)
{
    if (blob.size() < 16)
        return {};
    const int count = rdU16(blob, 12);
    int o = 16;
    for (int i = 0; i < count; ++i) {
        if (o + 12 > blob.size())
            break;
        const int size = int(rdU32(blob, o));
        const quint32 key = rdU32(blob, o + 4);
        o += 12;
        if (size < 1 || o + size > blob.size())
            break;
        if (key == kKeyStyleData) {
            QByteArray out = blob;
            QByteArray data = out.mid(o, size);
            patchViewTextStyles(data, vs);
            out.replace(o, size, data);
            return out;
        }
        o += size;
        if (size % 2 != 0)
            ++o;
    }
    return {};
}

// ---- COLUMN_PROPERTIES (per-task exceptional styles) ----------------------------

void readColumnProperties(const QByteArray &d, schedule::Project *out)
{
    QHash<int, int> rowByUid;
    for (int i = 0; i < out->tasks.size(); ++i)
        rowByUid.insert(out->tasks[i].uniqueId, i);

    // One record per (uid, field). Collapse to a row-level style: a whole-row
    // record (field 0xFFFFFFFF) is the row's format when present; otherwise
    // the Name column's record stands in (the common per-cell case).
    QHash<int, schedule::TextStyle> byUid;
    QHash<int, int> rank;   // 3 whole-row, 2 Name, 1 other cell
    const int count = d.size() / 44;
    for (int i = 0; i < count; ++i) {
        const int o = i * 44;
        const int uid = int(rdU32(d, o));
        if (!rowByUid.contains(uid))
            continue;
        const quint32 field = rdU32(d, o + 4);
        const bool wholeRow = field == kFieldWholeRow;
        if (!wholeRow && (field >> 16) != (kTaskFieldBase >> 16))
            continue;

        // The change mask at +40 gates every property: bits/colours outside
        // it are leftovers (e.g. a row record written only for a row-height
        // change carries pattern=1 with everything automatic -- applying that
        // at face value used to smear junk styles over whole rows).
        const quint16 mask = rdU16(d, o + 40);
        if ((mask & kChgAnyStyle) == 0)
            continue;

        const int newRank = wholeRow ? 3 : (quint16(field & 0xFFFF) == kFieldName ? 2 : 1);
        if (newRank <= rank.value(uid, 0))
            continue;

        const int bits = uchar(d.at(o + 11));
        schedule::TextStyle s;
        s.fontBaseIndex = uchar(d.at(o + 8));
        s.bold = (mask & kChgBold) && (bits & 0x01);
        s.italic = (mask & kChgItalic) && (bits & 0x02);
        s.underline = (mask & kChgUnderline) && (bits & 0x04);
        s.strikethrough = (mask & kChgStrike) && (bits & 0x08);
        if (mask & kChgColor)
            s.color = rdColor(d, o + 12);
        if (mask & kChgBackColor)
            s.backColor = rdColor(d, o + 24);
        if (mask & (kChgBackColor | kChgPattern))
            s.backPattern = rdU16(d, o + 36);
        if (s.backPattern == 0)   // transparent: any stored colour is not drawn
            s.backColor = schedule::TextStyle::kAutomatic;
        byUid.insert(uid, s);
        rank.insert(uid, newRank);
    }

    for (auto it = byUid.constBegin(); it != byUid.constEnd(); ++it)
        out->tasks[rowByUid.value(it.key())].rowFormat = it.value();
}

QByteArray buildColumnProperties(const schedule::Project &in)
{
    QByteArray d;
    for (const schedule::Task &t : in.tasks) {
        const schedule::TextStyle &s = t.rowFormat;
        if (s.isDefault())
            continue;

        // Byte-for-byte the shape MS Project writes for a whole-row format
        // (SelectRow + Font32Ex ground truth): an ID-column record plus a
        // whole-row (0xFFFFFFFF) record. Offset 8 is an index into the view
        // font-base table ("   214/Props" key 0x03400000, 68-byte entries
        // [flags u16][pointSize u16][name utf16x32]); base 3 is the stock
        // "Calibri 11" row default. Offsets 40-41 are the change mask gating
        // what the record applies -- without the right bits MS Project
        // renders none of it (this was the long-standing "formatting doesn't
        // render" bug). The stored pattern defaults to 1 (solid) and only
        // carries the kChgPattern bit when it is something else.
        const quint16 pat = s.backPattern == 0 ? 1 : quint16(s.backPattern);
        quint16 changeMask = 0;
        if (s.bold) changeMask |= kChgBold;
        if (s.underline) changeMask |= kChgUnderline;
        if (s.italic) changeMask |= kChgItalic;
        if (s.color != schedule::TextStyle::kAutomatic) changeMask |= kChgColor;
        if (s.backColor != schedule::TextStyle::kAutomatic) changeMask |= kChgBackColor;
        if (pat != 1) changeMask |= kChgPattern;
        if (s.strikethrough) changeMask |= kChgStrike;

        const quint32 fields[] = { kTaskFieldBase | kFieldId, kFieldWholeRow };
        for (quint32 field : fields) {
            QByteArray rec(44, '\0');
            wrU32(rec, 0, quint32(t.uniqueId));
            wrU32(rec, 4, field);
            rec[8] = char(s.fontBaseIndex >= 0 && s.fontBaseIndex <= 255
                          ? s.fontBaseIndex : 3);
            rec[11] = char((s.bold ? 0x01 : 0) | (s.italic ? 0x02 : 0)
                           | (s.underline ? 0x04 : 0)
                           | (s.strikethrough ? 0x08 : 0));
            wrColor(rec, 12, s.color);
            wrColor(rec, 24, s.backColor);
            wrU16(rec, 36, pat);
            wrU16(rec, 40, changeMask);
            d += rec;
        }
    }
    return d;
}

// ---- VarMeta / Var2Data rebuild --------------------------------------------------

struct VarRecord {
    quint32 uid = 0;
    quint32 offset = 0;
    quint16 typeLow = 0;
    quint16 typeHigh = 0;
};

bool parseVarMeta(const QByteArray &meta, QVector<VarRecord> *out)
{
    if (meta.size() < 24 || rdU32(meta, 0) != 0xFADFADBAu)
        return false;
    const int count = int(rdU32(meta, 8));
    out->clear();
    out->reserve(count);
    for (int i = 0; i < count; ++i) {
        const int o = 24 + i * 12;
        if (o + 12 > meta.size())
            return false;
        VarRecord r;
        r.uid = rdU32(meta, o);
        r.offset = rdU32(meta, o + 4);
        r.typeLow = rdU16(meta, o + 8);
        r.typeHigh = rdU16(meta, o + 10);
        out->append(r);
    }
    return true;
}

} // namespace

// -----------------------------------------------------------------------------

namespace ViewFormat {

void read(const CompoundFile &cf, schedule::Project *out)
{
    if (!out || !cf.hasStorage({ kViewStorage, kCView }))
        return;

    // Preserve the source font table together with the indices decoded below.
    const QByteArray fontProps = cf.readStream({ kViewStorage, QStringLiteral("Props") });
    for (int o = 16; o + 12 <= fontProps.size(); ) {
        const quint32 len = rdU32(fontProps, o);
        const quint32 key = rdU32(fontProps, o + 4);
        o += 12;
        if (len > quint32(fontProps.size() - o))
            break;
        if (key == kKeyFontBases) {
            out->mppFontBases = fontProps.mid(o, int(len));
            break;
        }
        o += int(len);
    }

    const QByteArray fixedMeta = cf.readStream({ kViewStorage, kCView, QStringLiteral("FixedMeta") });
    const QByteArray fixedData = cf.readStream({ kViewStorage, kCView, QStringLiteral("FixedData") });

    BkndVarData vd;
    if (!vd.parse(cf.readStream({ kViewStorage, kCView, QStringLiteral("VarMeta") }),
                  cf.readStream({ kViewStorage, kCView, QStringLiteral("Var2Data") })))
        return;

    // The Gantt Chart view: full template (text styles + gridlines + bars) plus
    // the per-task exceptional styles. The other views' styles are not read back
    // into the model: a written file always carries all standard views (from the
    // template), so reading their styles would make read(write(x)) != x for any
    // source that lacked them. Their styles are write-only -- patched into the
    // output when the app has set them (see patch()).
    const int ganttUid = findViewUid(fixedMeta, fixedData, kViewTypeGantt);
    if (ganttUid < 0)
        return;
    Props9 props;
    if (!parseProps9(vd.blobFor(quint32(ganttUid), kViewPropsType), &props))
        return;
    if (const PropsItem *style = props.find(kKeyStyleData))
        readStyleData(style->data, &out->viewStyles);
    if (const PropsItem *cols = props.find(kKeyColumnProperties))
        readColumnProperties(cols->data, out);
}

bool wantsPatch(const schedule::Project &in)
{
    if (in.viewStyles.present || in.resourceUsageStyles.present
        || in.teamPlannerStyles.present || in.calendarStyles.present)
        return true;
    for (const schedule::Task &t : in.tasks)
        if (!t.rowFormat.isDefault())
            return true;
    return false;
}

bool patch(const CompoundFile &tpl, CompoundFile &out, const schedule::Project &in)
{
    if (!tpl.hasStorage({ kViewStorage, kCView }))
        return false;

    const QByteArray fixedMeta = tpl.readStream({ kViewStorage, kCView, QStringLiteral("FixedMeta") });
    const QByteArray fixedData = tpl.readStream({ kViewStorage, kCView, QStringLiteral("FixedData") });
    const QByteArray varMeta = tpl.readStream({ kViewStorage, kCView, QStringLiteral("VarMeta") });
    const QByteArray var2 = tpl.readStream({ kViewStorage, kCView, QStringLiteral("Var2Data") });

    QVector<VarRecord> records;
    if (!parseVarMeta(varMeta, &records))
        return false;

    auto recordForView = [&](int uid) -> int {
        for (int i = 0; i < records.size(); ++i)
            if (records[i].uid == quint32(uid) && records[i].typeLow == kViewPropsType)
                return i;
        return -1;
    };
    auto parseRecordProps = [&](int recIdx, Props9 *p) -> bool {
        const int off = int(records[recIdx].offset);
        const int len = int(rdU32(var2, off));
        if (len <= 0 || off + 4 + len > var2.size())
            return false;
        return parseProps9(var2.mid(off + 4, len), p);
    };

    // recordIndex -> rebuilt Props9 blob.
    QHash<int, QByteArray> patched;

    // Gantt Chart view: full style template + per-task exceptional styles.
    const int ganttUid = findViewUid(fixedMeta, fixedData, kViewTypeGantt);
    const int ganttRec = ganttUid >= 0 ? recordForView(ganttUid) : -1;
    if (ganttRec >= 0) {
        Props9 props;
        if (parseRecordProps(ganttRec, &props)) {
            if (in.viewStyles.present)
                if (PropsItem *style = props.find(kKeyStyleData))
                    patchStyleData(style->data, in.viewStyles);

            const QByteArray colProps = buildColumnProperties(in);
            if (PropsItem *cols = props.find(kKeyColumnProperties)) {
                if (colProps.isEmpty()) {
                    for (int i = 0; i < props.items.size(); ++i)
                        if (props.items[i].key == kKeyColumnProperties) {
                            props.items.removeAt(i);
                            break;
                        }
                } else {
                    cols->data = colProps;
                    if (cols->flags == 0)
                        cols->flags = 1;   // heal a previously-written zero-flags item
                }
            } else if (!colProps.isEmpty()) {
                PropsItem item;
                item.key = kKeyColumnProperties;
                // Every Props9 item in real MS-Project-authored Gantt views carries
                // a nonzero flags value; match that rather than default to 0.
                item.flags = 1;
                item.data = colProps;
                props.items.append(item);
            }
            patched.insert(ganttRec, buildProps9(props));
        }
    }

    // Resource Usage / Team Planner / Calendar views: text styles only, patched
    // in place so the rest of each view's (undecoded) Props9 stays byte-identical.
    const struct { quint16 type; const schedule::ViewStyles *styles; } kOthers[] = {
        { kViewTypeResourceUsage, &in.resourceUsageStyles },
        { kViewTypeTeamPlanner, &in.teamPlannerStyles },
        { kViewTypeCalendar, &in.calendarStyles },
    };
    for (const auto &v : kOthers) {
        if (!v.styles->present)
            continue;
        const int uid = findViewUid(fixedMeta, fixedData, v.type);
        const int rec = uid >= 0 ? recordForView(uid) : -1;
        if (rec < 0 || patched.contains(rec))
            continue;
        const int off = int(records[rec].offset);
        const int len = int(rdU32(var2, off));
        if (len <= 0 || off + 4 + len > var2.size())
            continue;
        const QByteArray newBlob = patchViewTextStylesInBlob(var2.mid(off + 4, len), *v.styles);
        if (!newBlob.isEmpty())
            patched.insert(rec, newBlob);
    }

    if (patched.isEmpty())
        return false;   // nothing to change; caller copies the template verbatim

    // Rebuild Var2Data with every blob verbatim (in record order) except the
    // patched props blobs, recomputing offsets; then rebuild VarMeta to match.
    QByteArray newVar2;
    QByteArray newMeta = varMeta.left(24);
    for (int i = 0; i < records.size(); ++i) {
        const VarRecord &r = records[i];
        const int oldOff = int(r.offset);
        const int len = int(rdU32(var2, oldOff));
        QByteArray blob;
        if (patched.contains(i))
            blob = patched.value(i);
        else if (len >= 0 && oldOff + 4 + len <= var2.size())
            blob = var2.mid(oldOff + 4, len);

        const quint32 newOff = quint32(newVar2.size());
        appU32(newVar2, quint32(blob.size()));
        newVar2 += blob;

        appU32(newMeta, r.uid);
        appU32(newMeta, newOff);
        appU16(newMeta, r.typeLow);
        appU16(newMeta, r.typeHigh);
    }
    wrU32(newMeta, 20, quint32(newVar2.size()));   // header dataSize

    out.addStream({ kViewStorage, kCView, QStringLiteral("VarMeta") }, newMeta);
    out.addStream({ kViewStorage, kCView, QStringLiteral("Var2Data") }, newVar2);
    return true;
}

} // namespace ViewFormat
