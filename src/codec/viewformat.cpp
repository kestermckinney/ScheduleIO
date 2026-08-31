// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/viewformat.h"

#include "codec/bkndvardata.h"
#include "codec/mppfieldids.h"
#include "model/project.h"
#include "ole/compoundfile.h"

#include <QDate>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cstring>

namespace {

const QString kViewStorage = QStringLiteral("   214");
const QString kCView = QStringLiteral("CV_iew");
const QString kCTable = QStringLiteral("CTable");
const QString kExtensionStorage = QStringLiteral("ScheduleIO");
const QString kTaskUsageSelection = QStringLiteral("TaskUsageDetailSelection");
const QString kResourceUsageSelection = QStringLiteral("ResourceUsageDetailSelection");

// Props9 item keys (MPXJ PropsKey).
constexpr quint32 kKeyStyleData = 574619656u;         // STYLE_DATA
constexpr quint32 kKeyColumnProperties = 574619660u;  // COLUMN_PROPERTIES
constexpr quint32 kKeyBarExceptions = 574619661u;     // BAR_EXCEPTION_STYLES
constexpr quint32 kKeyTableName = 574619658u;         // TABLE_NAME
constexpr quint32 kKeyTableProperties = 574619655u;   // TABLE_PROPERTIES
constexpr quint32 kKeyViewFields = 574619708u;        // VIEW_FIELDS (Usage details)
constexpr quint32 kKeyFontBases = 54525952u;          // 0x03400000, in 214/Props

constexpr quint16 kViewPropsType = 6;   // GanttChartView14.PROPERTIES
// MPP view-type codes (u16@112 of the FixedData record). These do NOT match
// MPXJ's ViewType enum ordinals -- verified against real files by view name.
constexpr quint16 kViewTypeGantt = 1;
constexpr quint16 kViewTypeResourceUsage = 15;
constexpr quint16 kViewTypeTaskUsage = 14;
constexpr quint16 kViewTypeTeamPlanner = 9;
constexpr quint16 kViewTypeCalendar = 13;
constexpr quint16 kViewTypeTimeline = 16;
constexpr int kViewRecordSize = 138;    // CV_iew FixedData block size

// The Timeline view is a self-describing UTF-16 "<TLViewData>" XML document held
// identically in two places: the CV_iew Var2Data record of type 47 keyed by the
// timeline view uid, and Props9 item key 574619695 of that view's PROPERTIES.
// See src/model/timelineviewsettings.h and memory reference-mpp-timeline-tlviewdata.
constexpr quint16 kTimelineXmlVarType = 47;
constexpr quint32 kKeyTimelineXml = 574619695u;   // 0x2240000F
constexpr quint32 kTimelineSentinelUid = 4294967295u;   // template rows / "none"

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
constexpr int kTimescaleSizeOffset = 1180;
constexpr int kUsageFieldCountOffset = 2236;
constexpr int kUsageFieldsOffset = 2237;
constexpr int kTableRecordSize = 230;
constexpr int kTableColumnHeaderSize = 12;
constexpr int kTableColumnSize = 115;

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
                                 | kChgColor | kChgFontBase | kChgBackColor
                                 | kChgPattern | kChgStrike;

constexpr int kFontBaseHeaderSize = 2;
constexpr int kFontBaseRecordSize = 68;

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

void wrUtf16(QByteArray &b, int o, int maxBytes, const QString &value)
{
    if (o < 0 || maxBytes <= 0 || o + maxBytes > b.size())
        return;
    memset(b.data() + o, 0, size_t(maxBytes));
    const int chars = qMin(value.size(), maxBytes / 2 - 1);
    for (int i = 0; i < chars; ++i)
        wrU16(b, o + i * 2, value.at(i).unicode());
}

int fontBaseCount(const QByteArray &bases)
{
    if (bases.size() < kFontBaseHeaderSize)
        return 0;
    return qMin(int(rdU16(bases, 0)),
                (bases.size() - kFontBaseHeaderSize) / kFontBaseRecordSize);
}

QString fontBaseName(const QByteArray &bases, int index)
{
    if (index < 0 || index >= fontBaseCount(bases))
        return {};
    return rdUtf16(bases, kFontBaseHeaderSize + index * kFontBaseRecordSize + 4, 64);
}

int fontBaseSize(const QByteArray &bases, int index)
{
    if (index < 0 || index >= fontBaseCount(bases))
        return 0;
    return int(rdU16(bases, kFontBaseHeaderSize + index * kFontBaseRecordSize + 2));
}

void hydrateFont(schedule::TextStyle &style, const QByteArray &bases)
{
    if (style.fontBaseIndex < 0 || style.fontBaseIndex >= fontBaseCount(bases))
        return;
    style.fontName = fontBaseName(bases, style.fontBaseIndex);
    style.fontSize = fontBaseSize(bases, style.fontBaseIndex);
}

int ensureFontBase(QByteArray &bases, const schedule::TextStyle &style)
{
    if (style.fontBaseIndex >= 0 && style.fontBaseIndex < fontBaseCount(bases))
        return style.fontBaseIndex; // exact on-disk identity is authoritative
    if (style.fontName.isEmpty() && style.fontSize <= 0)
        return style.fontBaseIndex;
    const int count = fontBaseCount(bases);
    if (count == 0)
        return style.fontBaseIndex;

    const int inherited = qMin(3, count - 1);
    const QString family = style.fontName.isEmpty() ? fontBaseName(bases, inherited)
                                                    : style.fontName;
    const int points = style.fontSize > 0 ? style.fontSize : fontBaseSize(bases, inherited);
    for (int i = 0; i < count; ++i)
        if (fontBaseSize(bases, i) == points
            && fontBaseName(bases, i).compare(family, Qt::CaseInsensitive) == 0)
            return i;

    for (int i = 0; i < count; ++i) {
        if (!fontBaseName(bases, i).isEmpty() || fontBaseSize(bases, i) != 0)
            continue;
        const int o = kFontBaseHeaderSize + i * kFontBaseRecordSize;
        wrU16(bases, o, 0x0005); // MS Project's user-created font-base flags
        wrU16(bases, o + 2, quint16(qBound(1, points, 32767)));
        wrUtf16(bases, o + 4, 64, family);
        return i;
    }
    return inherited;
}

// Task::cellFormats uses ScheduleVault's stable numeric ColType keys for
// built-ins and "c:<name>" for custom fields. Keep this mapping beside the
// MPP FIELD_ARRAY indices so the codec can persist those cells independently.
int formatKeyForField(quint16 field)
{
    switch (field) {
    case 23: return 1;   // Id
    case 32: return 3;   // Percent
    case 14: return 4;   // Name
    case 29: return 5;   // Duration
    case 35: return 6;   // Start
    case 36: return 7;   // Finish
    case 47: return 8;   // Predecessors
    case 0:  return 9;   // Work
    case 16: return 10;  // WBS
    case 49: return 11;  // Resource Names
    case 5:  return 12;  // Cost
    case 8:  return 13;  // Fixed Cost
    case 7:  return 14;  // Actual Cost
    case 10: return 15;  // Remaining Cost
    case 9:  return 16;  // Cost Variance
    case 43: return 17;  // Baseline Start
    case 44: return 18;  // Baseline Finish
    case 27: return 19;  // Baseline Duration
    case 1:  return 20;  // Baseline Work
    case 6:  return 21;  // Baseline Cost
    case 41: return 22;  // Actual Start
    case 42: return 23;  // Actual Finish
    case 28: return 24;  // Actual Duration
    case 2:  return 25;  // Actual Work
    case 12: return 26;  // PV / BCWS
    case 11: return 27;  // EV / BCWP
    case 120:return 28;  // AC / ACWP
    case 83: return 29;  // CV
    case 13: return 30;  // SV
    case 537:return 31;  // CPI
    case 538:return 32;  // SPI
    case 541:return 34;  // EAC
    case 542:return 36;  // TCPI
    default: return -1;
    }
}

bool fieldForFormatKey(const QString &key, quint16 *field)
{
    if (key.startsWith(QLatin1String("c:"))) {
        const QString name = key.mid(2);
        for (const MppFieldIds::CustomFieldDef &def : MppFieldIds::taskCustomFields())
            if (name.compare(QLatin1String(def.name), Qt::CaseInsensitive) == 0) {
                *field = def.index;
                return true;
            }
        return false;
    }
    bool ok = false;
    const int col = key.toInt(&ok);
    if (!ok)
        return false;
    static const QHash<int, quint16> fields = {
        {1,23}, {3,32}, {4,14}, {5,29}, {6,35}, {7,36}, {8,47}, {9,0},
        {10,16}, {11,49}, {12,5}, {13,8}, {14,7}, {15,10}, {16,9},
        {17,43}, {18,44}, {19,27}, {20,1}, {21,6}, {22,41}, {23,42},
        {24,28}, {25,2}, {26,12}, {27,11}, {28,120}, {29,83}, {30,13},
        {31,537}, {32,538}, {34,541}, {36,542}
    };
    const auto it = fields.constFind(col);
    if (it == fields.cend())
        return false;
    *field = it.value();
    return true;
}

QString formatKeyForMppField(quint16 field)
{
    const int builtIn = formatKeyForField(field);
    if (builtIn >= 0)
        return QString::number(builtIn);
    for (const MppFieldIds::CustomFieldDef &def : MppFieldIds::taskCustomFields())
        if (def.index == field)
            return QStringLiteral("c:") + QLatin1String(def.name);
    return {};
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

QByteArray findViewRecord(const QByteArray &fixedMeta, const QByteArray &fixedData,
                          quint16 viewType);

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

QString normalizedPresentationName(QString name)
{
    name.remove(QLatin1Char('&'));
    return name.trimmed().toCaseFolded();
}

void readUsageView(const CompoundFile &cf, const QByteArray &viewFixedMeta,
                   const QByteArray &viewFixedData, const BkndVarData &viewVarData,
                   quint16 viewType, schedule::UsageViewSettings *out)
{
    if (!out || !cf.hasStorage({ kViewStorage, kCTable }))
        return;

    const QByteArray viewRecord = findViewRecord(viewFixedMeta, viewFixedData, viewType);
    if (viewRecord.isEmpty())
        return;
    const quint32 viewUid = rdU32(viewRecord, 0);
    Props9 props;
    if (!parseProps9(viewVarData.blobFor(viewUid, kViewPropsType), &props))
        return;
    const PropsItem *tableNameItem = props.find(kKeyTableName);
    if (!tableNameItem)
        return;

    out->viewName = rdUtf16(viewRecord, 4, 104);
    out->tableName = rdUtf16(tableNameItem->data, 0, tableNameItem->data.size());
    if (const PropsItem *table = props.find(kKeyTableProperties)) {
        if (table->data.size() >= 37)
            out->tableWidth = rdU16(table->data, 35);
    }
    if (const PropsItem *style = props.find(kKeyStyleData)) {
        if (style->data.size() > kTimescaleSizeOffset)
            out->timescaleSize = quint8(style->data.at(kTimescaleSizeOffset));
        if ((viewType == kViewTypeResourceUsage || viewType == kViewTypeTaskUsage)
            && style->data.size() > kUsageFieldCountOffset) {
            const int count = quint8(style->data.at(kUsageFieldCountOffset));
            if (count > 0 && kUsageFieldsOffset + count <= style->data.size()) {
                out->detailFields.clear();
                for (int i = 0; i < count; ++i)
                    out->detailFields.append(quint8(style->data.at(kUsageFieldsOffset + i)));
            }
        }
    }
    if (out->detailFields.isEmpty()
        && (viewType == kViewTypeResourceUsage || viewType == kViewTypeTaskUsage)) {
        if (const PropsItem *fields = props.find(kKeyViewFields)) {
            out->detailFields.clear();
            for (char value : fields->data) {
                const int field = quint8(value);
                if (field == 0xFF)
                    break;
                out->detailFields.append(field);
            }
        }
    }

    const QByteArray tableFixed = cf.readStream(
        { kViewStorage, kCTable, QStringLiteral("FixedData") });
    BkndVarData tableVars;
    if (!tableVars.parse(cf.readStream({ kViewStorage, kCTable, QStringLiteral("VarMeta") }),
                         cf.readStream({ kViewStorage, kCTable, QStringLiteral("Var2Data") })))
        return;

    const QString wanted = normalizedPresentationName(out->tableName);
    for (int offset = 0; offset + kTableRecordSize <= tableFixed.size();
         offset += kTableRecordSize) {
        const QByteArray rec = tableFixed.mid(offset, kTableRecordSize);
        const quint32 tableUid = rdU32(rec, 0);
        if (normalizedPresentationName(rdUtf16(rec, 4, 104)) != wanted)
            continue;
        const bool resourceTable = rdU16(rec, 108) == 1;
        if (resourceTable != (viewType == kViewTypeResourceUsage
                              || viewType == kViewTypeTeamPlanner))
            continue;

        QByteArray columns;
        for (quint16 type : { quint16(8), quint16(7), quint16(6) }) {
            columns = tableVars.blobFor(tableUid, type);
            if (!columns.isEmpty())
                break;
        }
        if (columns.size() < kTableColumnHeaderSize)
            return;
        const int count = int(rdU16(columns, 4)) + 1;
        out->columns.clear();
        for (int i = 0; i < count; ++i) {
            const int at = kTableColumnHeaderSize + i * kTableColumnSize;
            if (at + kTableColumnSize > columns.size())
                break;
            schedule::UsageTableColumn column;
            column.fieldId = rdU32(columns, at);
            column.width = quint8(columns.at(at + 4));
            column.title = rdUtf16(columns, at + 13, 100);
            out->columns.append(column);
        }
        out->present = !out->columns.isEmpty();
        return;
    }
}

// ---- COLUMN_PROPERTIES (per-task exceptional styles) ----------------------------

void readColumnProperties(const QByteArray &d, schedule::Project *out)
{
    QHash<int, int> rowByUid;
    for (int i = 0; i < out->tasks.size(); ++i)
        rowByUid.insert(out->tasks[i].uniqueId, i);

    QHash<int, schedule::TextStyle> rowsByUid;
    QHash<int, QHash<QString, schedule::TextStyle>> cellsByUid;
    QSet<int> wholeRowUids;
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

        const int bits = uchar(d.at(o + 11));
        schedule::TextStyle s;
        if (mask & kChgFontBase)
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
        if (wholeRow) {
            rowsByUid.insert(uid, s);
            wholeRowUids.insert(uid);
        } else {
            const QString key = formatKeyForMppField(quint16(field & 0xFFFF));
            if (!key.isEmpty())
                cellsByUid[uid].insert(key, s);
        }
    }

    for (auto it = rowsByUid.constBegin(); it != rowsByUid.constEnd(); ++it)
        out->tasks[rowByUid.value(it.key())].rowFormat = it.value();
    for (auto taskIt = cellsByUid.constBegin(); taskIt != cellsByUid.constEnd(); ++taskIt) {
        schedule::Task &task = out->tasks[rowByUid.value(taskIt.key())];
        for (auto cellIt = taskIt->constBegin(); cellIt != taskIt->constEnd(); ++cellIt) {
            // A whole-row format carries a duplicate ID-column record. It is
            // structural, not a real per-cell override.
            if (wholeRowUids.contains(taskIt.key()) && cellIt.key() == QLatin1String("1"))
                continue;
            task.cellFormats.insert(cellIt.key(), cellIt.value());
            // Preserve the historical Name-cell fallback used by existing
            // formatting fixtures while also retaining the true cell format.
            if (!wholeRowUids.contains(taskIt.key()) && cellIt.key() == QLatin1String("4"))
                task.rowFormat = cellIt.value();
        }
    }
}

QByteArray findViewRecord(const QByteArray &fixedMeta, const QByteArray &fixedData,
                          quint16 viewType)
{
    if (fixedMeta.size() < 16)
        return {};
    const int items = int(rdU32(fixedMeta, 8));
    int lastOffset = -1;
    for (int i = 0; i < items; ++i) {
        const int metaOff = 16 + i * 10;
        if (metaOff + 10 > fixedMeta.size())
            break;
        const int offset = int(rdU16(fixedMeta, metaOff + 4));
        if (offset <= lastOffset)
            continue;
        lastOffset = offset;
        if (offset + kViewRecordSize > fixedData.size())
            continue;
        const QByteArray rec = fixedData.mid(offset, kViewRecordSize);
        if (rdU16(rec, 110) == 0 && rdU16(rec, 112) == viewType)
            return rec;
    }
    return {};
}

// ---- Per-task bar formatting (BAR_EXCEPTION_STYLES) ---------------------------
//
// Format > Bar on a single task, as opposed to Format > Bar Styles which edits a
// whole category. The Gantt view's Props9 item 574619661 is a bare array of
// 71-byte records, one per formatted task, sorted by task unique id, and absent
// entirely when nothing is formatted. Layout (established against files MS
// Project itself wrote, sweeping GanttBarFormat over the colour, shape, pattern
// and text arguments):
//
//     +0   u32  task unique id
//     +4   u16  the bar style the exception is based on (0 for one Project
//               writes for a plain colour change)
//     +6   u8   middle shape
//     +7   u8   middle pattern
//     +8   4    middle colour (r,g,b + automatic flag), our Task::barColor
//     +20  u8   start shape (v % 21) and type (v / 21)
//     +21  4    start colour
//     +33  u8   end shape and type
//     +34  4    end colour
//     +49  5x4  left/right/top/bottom/inside bar text, each a task field id
//               (0x0B400000 | field index) or 0xFFFFFFFF for none
//     +69  u16  trailing flag (0 or 2 in the wild)
//
// The gaps are zero in every sample and are carried through untouched, as is
// everything but the middle colour: the record replaces a task's whole bar, so
// dropping the parts we don't model would silently restyle bars formatted in
// Project.
constexpr int kBarExcSize = 71;
constexpr int kBarExcUid = 0;
constexpr int kBarExcMiddleShape = 6;
constexpr int kBarExcMiddlePattern = 7;
constexpr int kBarExcMiddleColor = 8;
constexpr int kBarExcRightText = 53;
constexpr int kBarExcTrailing = 69;

// What MS Project writes into a brand-new exception when only the colour was
// changed: a solid rectangle carrying the stock Gantt bar's right-hand text.
constexpr quint8 kBarExcDefaultShape = 1;
constexpr quint8 kBarExcDefaultPattern = 1;
constexpr quint32 kBarExcDefaultRightText = 0x0B400031u;   // field 49
constexpr quint16 kBarExcDefaultTrailing = 2;

void readBarExceptions(const QByteArray &d, schedule::Project *out)
{
    QHash<int, int> rowByUid;
    for (int i = 0; i < out->tasks.size(); ++i)
        rowByUid.insert(out->tasks[i].uniqueId, i);

    const int count = d.size() / kBarExcSize;
    for (int i = 0; i < count; ++i) {
        const int o = i * kBarExcSize;
        const int uid = int(rdU32(d, o + kBarExcUid));
        const auto row = rowByUid.constFind(uid);
        if (row == rowByUid.constEnd())
            continue;
        // An all-zero colour is Project's "no override": a record can exist to
        // carry a shape or a bar text on its own. Verified against Project
        // itself -- a task whose record reads 00 00 00 00 draws the ordinary
        // bar colour (#8ABBED on the stock Gantt), not black. The cost is that
        // black is not expressible here; Project's own colour argument treats 0
        // as automatic too, so it cannot set a black bar either.
        const qint32 color = rdColor(d, o + kBarExcMiddleColor);
        if (color == 0)
            continue;
        out->tasks[row.value()].barColor = color;
    }
}

QByteArray buildBarExceptions(const schedule::Project &in)
{
    // Start from the source file's own array so a task Project formatted keeps
    // its shapes, ends and bar text; only the colour is ours to say anything
    // about.
    QByteArray d = in.mppBarExceptions;
    if (d.size() % kBarExcSize != 0)
        d.clear();

    QHash<int, int> recordByUid;
    for (int i = 0; i < d.size() / kBarExcSize; ++i)
        recordByUid.insert(int(rdU32(d, i * kBarExcSize + kBarExcUid)), i);

    for (const schedule::Task &task : in.tasks) {
        const auto existing = recordByUid.constFind(task.uniqueId);
        if (existing != recordByUid.constEnd()) {
            // Clearing a colour leaves the record in place with the colour set
            // back to automatic: it may still carry shape or text formatting,
            // and dropping it would take that with it.
            wrColor(d, existing.value() * kBarExcSize + kBarExcMiddleColor, task.barColor);
            continue;
        }
        if (task.barColor == schedule::TextStyle::kAutomatic)
            continue;

        QByteArray record(kBarExcSize, '\0');
        wrU32(record, kBarExcUid, quint32(task.uniqueId));
        // Style id stays 0, byte-for-byte what Project itself writes for a
        // colour-only exception (verified by diffing our record against one
        // Project authored for the same task and colour: identical).
        record[kBarExcMiddleShape] = char(kBarExcDefaultShape);
        record[kBarExcMiddlePattern] = char(kBarExcDefaultPattern);
        wrColor(record, kBarExcMiddleColor, task.barColor);
        for (int text = 49; text < 69; text += 4)
            wrU32(record, text, 0xFFFFFFFFu);
        wrU32(record, kBarExcRightText, kBarExcDefaultRightText);
        wrU16(record, kBarExcTrailing, kBarExcDefaultTrailing);
        recordByUid.insert(task.uniqueId, d.size() / kBarExcSize);
        d.append(record);
    }

    // Project keeps the array in task-unique-id order; appended records have to
    // be sorted back in or it re-sorts (and rewrites) the whole item on open.
    const int count = d.size() / kBarExcSize;
    QVector<QByteArray> records;
    records.reserve(count);
    for (int i = 0; i < count; ++i)
        records.append(d.mid(i * kBarExcSize, kBarExcSize));
    std::sort(records.begin(), records.end(),
              [](const QByteArray &a, const QByteArray &b) {
        return rdU32(a, kBarExcUid) < rdU32(b, kBarExcUid);
    });
    QByteArray sorted;
    sorted.reserve(d.size());
    for (const QByteArray &record : records)
        sorted.append(record);
    return sorted;
}

QByteArray buildColumnProperties(const schedule::Project &in)
{
    QByteArray d;
    const auto appendRecord = [&](int uid, quint32 field, const schedule::TextStyle &s,
                                  bool explicitRowWeight = false) {
        const quint16 pat = s.backPattern == 0 ? 1 : quint16(s.backPattern);
        quint16 changeMask = 0;
        // A Project-authored whole-row Font32Ex record always carries the bold
        // change bit, even when its value is false. Without that explicit
        // "normal weight" override, Project renders formatted task rows bold.
        if (explicitRowWeight || s.bold) changeMask |= kChgBold;
        if (s.underline) changeMask |= kChgUnderline;
        if (s.italic) changeMask |= kChgItalic;
        if (s.color != schedule::TextStyle::kAutomatic) changeMask |= kChgColor;
        if (s.fontBaseIndex >= 0) changeMask |= kChgFontBase;
        if (s.backColor != schedule::TextStyle::kAutomatic) changeMask |= kChgBackColor;
        if (pat != 1) changeMask |= kChgPattern;
        if (s.strikethrough) changeMask |= kChgStrike;

        QByteArray rec(44, '\0');
        wrU32(rec, 0, quint32(uid));
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
    };

    for (const schedule::Task &t : in.tasks) {
        const schedule::TextStyle &s = t.rowFormat;
        if (!s.isDefault()) {
            // MS Project writes an ID-column record plus a whole-row record.
            appendRecord(t.uniqueId, kTaskFieldBase | kFieldId, s, true);
            appendRecord(t.uniqueId, kFieldWholeRow, s, true);
        }

        QStringList keys = t.cellFormats.keys();
        std::sort(keys.begin(), keys.end());
        for (const QString &key : keys) {
            const schedule::TextStyle cell = t.cellFormats.value(key);
            quint16 field = 0;
            if (cell.isDefault() || !fieldForFormatKey(key, &field))
                continue;
            appendRecord(t.uniqueId, kTaskFieldBase | field, cell);
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

// ---- Timeline ("<TLViewData>" XML) --------------------------------------------

// The UTF-16 view name held at offset 4..110 of a 138-byte CV_iew record.
QString viewRecordName(const QByteArray &rec)
{
    QString s;
    for (int o = 4; o + 1 < 110 && o + 1 < rec.size(); o += 2) {
        const ushort ch = rdU16(rec, o);
        if (ch == 0)
            break;
        s.append(QChar(ch));
    }
    return s;
}

// Decode a "<TLViewData>" UTF-16LE document into the model. Anything not modelled
// stays in TimelineViewSettings::rawXml so the writer can re-emit it verbatim.
void parseTimelineXml(const QByteArray &utf16, schedule::TimelineViewSettings *tv)
{
    if (utf16.size() < 4)
        return;
    const QString xml = QString::fromUtf16(
        reinterpret_cast<const char16_t *>(utf16.constData()), utf16.size() / 2);
    QXmlStreamReader r(xml);

    const auto boolAttr = [](const QXmlStreamAttributes &a, QLatin1String name, bool dflt) {
        const QStringView v = a.value(name);
        return v.isEmpty() ? dflt : (v != QLatin1String("0"));
    };
    const auto colorAttr = [](const QXmlStreamAttributes &a) -> qint32 {
        if (a.value(QLatin1String("thm")).toString() != QLatin1String("0000"))
            return schedule::TimelineTextStyle::kAutomatic;   // a theme colour
        bool ok = false;
        const quint32 argb = a.value(QLatin1String("clr")).toUInt(&ok, 16);
        return ok ? qint32(argb & 0xFFFFFFu) : schedule::TimelineTextStyle::kAutomatic;
    };
    const auto dateAttr = [](const QXmlStreamAttributes &a, QLatin1String name) {
        return QDate::fromString(a.value(name).toString(), QStringLiteral("yyyy/MM/dd"));
    };

    // MS Project marks a callout member with an <ft> row in <fltSet> (which is
    // emitted before <tskSet>), and drops its <tskSet> <t> row to onTL="0". The
    // <mlSet> <m> row is untouched. Collect the callout task uids first so the
    // <t> pass can tag them.
    QSet<int> calloutUids;

    while (!r.atEnd()) {
        if (r.readNext() != QXmlStreamReader::StartElement)
            continue;
        const QXmlStreamAttributes a = r.attributes();
        const QStringView name = r.name();

        if (name == QLatin1String("TLViewData")) {
            tv->dfltTLView = boolAttr(a, QLatin1String("dfltTLView"), true);
        } else if (name == QLatin1String("ft")) {           // a callout member (<fltSet>)
            const quint32 uid = a.value(QLatin1String("uid")).toUInt();
            if (uid != kTimelineSentinelUid && boolAttr(a, QLatin1String("onTL"), true))
                calloutUids.insert(int(uid));
        } else if (name == QLatin1String("tl")) {           // a timeline bar
            schedule::TimelineBar bar;
            bar.id = a.value(QLatin1String("id")).toInt();
            bar.label = a.value(QLatin1String("label")).toString();
            bar.useCustomDates = boolAttr(a, QLatin1String("useCustomDates"), false);
            bar.customStart = dateAttr(a, QLatin1String("startDate"));
            bar.customFinish = dateAttr(a, QLatin1String("finishDate"));
            tv->bars.append(bar);
        } else if (name == QLatin1String("t")) {            // a task member (<tskSet>)
            const quint32 uid = a.value(QLatin1String("uid")).toUInt();
            if (uid == kTimelineSentinelUid)
                continue;                                    // the template row
            schedule::TimelineItem it;
            it.guid = a.value(QLatin1String("id")).toString();
            it.taskUid = int(uid);
            it.barId = a.hasAttribute(QLatin1String("barid"))
                ? a.value(QLatin1String("barid")).toInt() : 1;
            it.onTimeline = boolAttr(a, QLatin1String("onTL"), true);
            if (calloutUids.contains(int(uid))) {
                // A callout: the <t> row carries onTL="0" but the task is on the
                // timeline, shown as a callout via its <ft> row.
                it.display = schedule::TimelineItemDisplay::Callout;
                it.onTimeline = true;
            }
            tv->items.append(it);
            // <mlSet> mirrors these rows; the writer re-emits both from `items`.
        } else if (name == QLatin1String("style")) {        // <txtSet>
            schedule::TimelineTextStyle s;
            s.id = a.value(QLatin1String("id")).toInt();
            s.type = a.value(QLatin1String("type")).toInt();
            s.color = colorAttr(a);
            s.fontName = a.value(QLatin1String("font")).toString();
            s.fontSize = a.value(QLatin1String("sz")).toInt();
            s.bold = boolAttr(a, QLatin1String("bold"), false);
            s.italic = boolAttr(a, QLatin1String("ital"), false);
            s.underline = boolAttr(a, QLatin1String("und"), false);
            s.strikethrough = boolAttr(a, QLatin1String("strk"), false);
            tv->textStyles.append(s);
        } else if (name == QLatin1String("options")) {
            if (a.hasAttribute(QLatin1String("dateFormat")))
                tv->dateFormat = a.value(QLatin1String("dateFormat")).toInt();
            if (a.hasAttribute(QLatin1String("numTextLines")))
                tv->numTextLines = a.value(QLatin1String("numTextLines")).toInt();
            // On disk the Today-line / timescale attribute names look transposed
            // relative to their meaning (see timelineviewsettings.h).
            tv->showTodayLine    = boolAttr(a, QLatin1String("showTS"), tv->showTodayLine);
            tv->showTimescale    = boolAttr(a, QLatin1String("showToday"), tv->showTimescale);
            tv->showPanZoom      = boolAttr(a, QLatin1String("showPanZoom"), tv->showPanZoom);
            tv->showDates        = boolAttr(a, QLatin1String("showDates"), tv->showDates);
            tv->showOverlaps     = boolAttr(a, QLatin1String("showOverlaps"), tv->showOverlaps);
            tv->showTaskProgress = boolAttr(a, QLatin1String("showTaskProgress"), tv->showTaskProgress);
        }
    }
}

QString timelineDateStr(const QDate &d)
{
    return d.isValid() ? d.toString(QStringLiteral("yyyy/MM/dd")) : QString();
}

QString timelineColorStr(qint32 rgb)
{
    return QStringLiteral("FF%1").arg(quint32(rgb) & 0xFFFFFFu, 6, 16, QChar('0')).toUpper();
}

// Re-emit the "<TLViewData>" document with the modelled fields brought up to date
// and everything else (the <fltSet>/<fmtSet> shape records, per-item fmt/ch/x/y
// attributes, unrecognised <options> attributes, ...) copied through untouched.
// Deterministic: iteration order over the model lists is stable and no value is
// hashed. Returns UTF-16LE bytes with no BOM / no XML declaration, as MS Project
// writes it.
QByteArray serializeTimelineXml(const schedule::TimelineViewSettings &tv)
{
    if (tv.rawXml.isEmpty())
        return {};
    QString src = QString::fromUtf16(
        reinterpret_cast<const char16_t *>(tv.rawXml.constData()), tv.rawXml.size() / 2);
    // The var blob can carry a trailing UTF-16 NUL (and/or padding) past the
    // closing tag; QXmlStreamReader would flag it as "extra content". Strip it
    // for parsing, then re-append the exact bytes so the re-emit keeps MS
    // Project's framing.
    QString trailer;
    while (!src.isEmpty() && (src.back() == QChar(0) || src.back().isSpace())) {
        trailer.prepend(src.back());
        src.chop(1);
    }

    QHash<int, const schedule::TimelineItem *> itemByUid;
    for (const schedule::TimelineItem &it : tv.items)
        itemByUid.insert(it.taskUid, &it);
    QHash<int, const schedule::TimelineBar *> barById;
    for (const schedule::TimelineBar &b : tv.bars)
        barById.insert(b.id, &b);
    QHash<int, const schedule::TimelineTextStyle *> styleById;
    for (const schedule::TimelineTextStyle &s : tv.textStyles)
        styleById.insert(s.id, &s);

    // A callout's <tskSet> row carries onTL="0" (the task rides in <fltSet>
    // instead); its <mlSet> row and a bar's rows all carry onTL="1".
    const auto onTlFor = [](const schedule::TimelineItem &it, const QString &tag) {
        const bool off = tag == QLatin1String("t")
            && it.display == schedule::TimelineItemDisplay::Callout;
        return (it.onTimeline && !off) ? QStringLiteral("1") : QStringLiteral("0");
    };
    const auto writeItem = [&onTlFor](QXmlStreamWriter &w, const QString &tag,
                              const schedule::TimelineItem &it) {
        w.writeEmptyElement(tag);
        w.writeAttribute(QStringLiteral("id"), it.guid);
        w.writeAttribute(QStringLiteral("uid"), QString::number(quint32(it.taskUid)));
        w.writeAttribute(QStringLiteral("onTL"), onTlFor(it, tag));
        w.writeAttribute(QStringLiteral("barid"), QString::number(it.barId));
    };
    // A callout member's <fltSet> row. Attribute order per MS Project:
    // id uid onTL top barid.
    const auto writeFt = [](QXmlStreamWriter &w, const schedule::TimelineItem &it) {
        w.writeEmptyElement(QStringLiteral("ft"));
        w.writeAttribute(QStringLiteral("id"), it.guid);
        w.writeAttribute(QStringLiteral("uid"), QString::number(quint32(it.taskUid)));
        w.writeAttribute(QStringLiteral("onTL"), QStringLiteral("1"));
        w.writeAttribute(QStringLiteral("top"), QStringLiteral("1"));
        w.writeAttribute(QStringLiteral("barid"), QString::number(it.barId));
    };
    // Attribute order matches MS Project: the internal bar 0 always spells out
    // every attribute with `label` last; a visible bar carries `label` right
    // after `id` (when set) and the date trio only when custom.
    const auto writeBar = [](QXmlStreamWriter &w, const schedule::TimelineBar &b) {
        w.writeEmptyElement(QStringLiteral("tl"));
        w.writeAttribute(QStringLiteral("id"), QString::number(b.id));
        if (b.id == 0) {
            w.writeAttribute(QStringLiteral("useCustomDates"),
                             b.useCustomDates ? QStringLiteral("1") : QStringLiteral("0"));
            w.writeAttribute(QStringLiteral("startDate"),
                             b.useCustomDates ? timelineDateStr(b.customStart) : QString());
            w.writeAttribute(QStringLiteral("finishDate"),
                             b.useCustomDates ? timelineDateStr(b.customFinish) : QString());
            w.writeAttribute(QStringLiteral("label"), b.label);
            return;
        }
        if (!b.label.isEmpty())
            w.writeAttribute(QStringLiteral("label"), b.label);
        if (b.useCustomDates) {
            w.writeAttribute(QStringLiteral("useCustomDates"), QStringLiteral("1"));
            w.writeAttribute(QStringLiteral("startDate"), timelineDateStr(b.customStart));
            w.writeAttribute(QStringLiteral("finishDate"), timelineDateStr(b.customFinish));
        }
    };

    QString outStr;
    QXmlStreamReader r(src);
    QXmlStreamWriter w(&outStr);
    w.setAutoFormatting(false);

    QSet<int> emittedItems;
    QSet<int> emittedBars;
    QSet<int> emittedFts;
    while (!r.atEnd()) {
        switch (r.readNext()) {
        case QXmlStreamReader::StartElement: {
            const QString n = r.name().toString();
            const QXmlStreamAttributes a = r.attributes();
            if (n == QLatin1String("t") || n == QLatin1String("m")) {
                const quint32 uid = a.value(QLatin1String("uid")).toUInt();
                if (uid == kTimelineSentinelUid) {
                    w.writeStartElement(n);
                    w.writeAttributes(a);           // the template row: verbatim
                    break;
                }
                const schedule::TimelineItem *mi = itemByUid.value(int(uid));
                if (!mi) {
                    r.skipCurrentElement();          // member was removed
                    break;
                }
                w.writeStartElement(n);
                bool sawBarid = false;
                bool sawOnTl = false;
                for (const QXmlStreamAttribute &at : a) {
                    if (at.name() == QLatin1String("barid")) {
                        w.writeAttribute(QStringLiteral("barid"), QString::number(mi->barId));
                        sawBarid = true;
                    } else if (at.name() == QLatin1String("onTL")) {
                        w.writeAttribute(QStringLiteral("onTL"), onTlFor(*mi, n));
                        sawOnTl = true;
                    } else {
                        w.writeAttribute(at.qualifiedName().toString(), at.value().toString());
                    }
                }
                if (!sawOnTl)
                    w.writeAttribute(QStringLiteral("onTL"), onTlFor(*mi, n));
                if (!sawBarid)
                    w.writeAttribute(QStringLiteral("barid"), QString::number(mi->barId));
                emittedItems.insert(int(uid));
            } else if (n == QLatin1String("ft")) {
                const quint32 uid = a.value(QLatin1String("uid")).toUInt();
                if (uid == kTimelineSentinelUid) {
                    w.writeStartElement(n);
                    w.writeAttributes(a);           // the template row: verbatim
                    break;
                }
                const schedule::TimelineItem *mi = itemByUid.value(int(uid));
                if (mi && mi->display == schedule::TimelineItemDisplay::Callout) {
                    writeFt(w, *mi);
                    emittedFts.insert(int(uid));
                }
                r.skipCurrentElement();             // no longer a callout / removed
            } else if (n == QLatin1String("tl")) {
                const int id = a.value(QLatin1String("id")).toInt();
                const schedule::TimelineBar *mb = barById.value(id);
                if (!mb) {
                    r.skipCurrentElement();          // bar was removed
                    break;
                }
                writeBar(w, *mb);
                emittedBars.insert(id);
                r.skipCurrentElement();
            } else if (n == QLatin1String("options")) {
                w.writeStartElement(QStringLiteral("options"));
                for (const QXmlStreamAttribute &at : a) {
                    const QString an = at.name().toString();
                    QString av = at.value().toString();
                    if (an == QLatin1String("showTS"))
                        av = tv.showTodayLine ? QStringLiteral("1") : QStringLiteral("0");
                    else if (an == QLatin1String("showToday"))
                        av = tv.showTimescale ? QStringLiteral("1") : QStringLiteral("0");
                    else if (an == QLatin1String("showPanZoom"))
                        av = tv.showPanZoom ? QStringLiteral("1") : QStringLiteral("0");
                    else if (an == QLatin1String("showOverlaps"))
                        av = tv.showOverlaps ? QStringLiteral("1") : QStringLiteral("0");
                    else if (an == QLatin1String("showDates"))
                        av = tv.showDates ? QStringLiteral("1") : QStringLiteral("0");
                    else if (an == QLatin1String("showTaskProgress"))
                        av = tv.showTaskProgress ? QStringLiteral("1") : QStringLiteral("0");
                    else if (an == QLatin1String("numTextLines"))
                        av = QString::number(tv.numTextLines);
                    else if (an == QLatin1String("dateFormat"))
                        av = QString::number(tv.dateFormat);
                    w.writeAttribute(an, av);
                }
            } else if (n == QLatin1String("style")) {
                const schedule::TimelineTextStyle *ms =
                    styleById.value(a.value(QLatin1String("id")).toInt());
                w.writeStartElement(QStringLiteral("style"));
                for (const QXmlStreamAttribute &at : a) {
                    const QString an = at.name().toString();
                    QString av = at.value().toString();
                    if (ms) {
                        if (an == QLatin1String("bold"))
                            av = ms->bold ? QStringLiteral("1") : QStringLiteral("0");
                        else if (an == QLatin1String("ital"))
                            av = ms->italic ? QStringLiteral("1") : QStringLiteral("0");
                        else if (an == QLatin1String("und"))
                            av = ms->underline ? QStringLiteral("1") : QStringLiteral("0");
                        else if (an == QLatin1String("strk"))
                            av = ms->strikethrough ? QStringLiteral("1") : QStringLiteral("0");
                        else if (an == QLatin1String("sz") && ms->fontSize > 0)
                            av = QString::number(ms->fontSize);
                        else if (an == QLatin1String("font") && !ms->fontName.isEmpty())
                            av = ms->fontName;
                        else if (an == QLatin1String("clr")
                                 && ms->color != schedule::TimelineTextStyle::kAutomatic)
                            av = timelineColorStr(ms->color);
                    }
                    w.writeAttribute(an, av);
                }
            } else {
                w.writeStartElement(n);
                w.writeAttributes(a);
            }
            break;
        }
        case QXmlStreamReader::EndElement: {
            const QString n = r.name().toString();
            if (n == QLatin1String("fltSet")) {
                for (const schedule::TimelineItem &it : tv.items)
                    if (it.display == schedule::TimelineItemDisplay::Callout
                        && !emittedFts.contains(it.taskUid))
                        writeFt(w, it);
                emittedFts.clear();
            } else if (n == QLatin1String("tskSet") || n == QLatin1String("mlSet")) {
                const QString tag = n == QLatin1String("tskSet") ? QStringLiteral("t")
                                                                : QStringLiteral("m");
                for (const schedule::TimelineItem &it : tv.items)
                    if (!emittedItems.contains(it.taskUid))
                        writeItem(w, tag, it);
                emittedItems.clear();
            } else if (n == QLatin1String("tlbarSet")) {
                for (const schedule::TimelineBar &b : tv.bars)
                    if (b.id >= 1 && !emittedBars.contains(b.id))
                        writeBar(w, b);
                emittedBars.clear();
            }
            w.writeEndElement();
            break;
        }
        case QXmlStreamReader::Characters:
            if (!r.isWhitespace())
                w.writeCharacters(r.text().toString());
            break;
        default:
            break;
        }
    }
    if (r.hasError())
        return {};
    outStr += trailer;

    QByteArray out;
    out.reserve(outStr.size() * 2);
    for (QChar c : outStr) {
        const ushort u = c.unicode();
        out.append(char(u & 0xFF));
        out.append(char((u >> 8) & 0xFF));
    }
    return out;
}

void readTimeline(const QByteArray &fixedMeta, const QByteArray &fixedData,
                  const BkndVarData &vd, schedule::Project *out)
{
    const int uid = findViewUid(fixedMeta, fixedData, kViewTypeTimeline);
    if (uid < 0)
        return;

    QByteArray xml = vd.blobFor(quint32(uid), kTimelineXmlVarType);
    if (xml.isEmpty()) {
        Props9 props;
        if (parseProps9(vd.blobFor(quint32(uid), kViewPropsType), &props))
            if (const PropsItem *item = props.find(kKeyTimelineXml))
                xml = item->data;
    }
    if (xml.isEmpty())
        return;

    schedule::TimelineViewSettings &tv = out->timelineView;
    tv = schedule::TimelineViewSettings();
    tv.present = true;
    tv.viewUid = uid;
    tv.viewName = viewRecordName(findViewRecord(fixedMeta, fixedData, kViewTypeTimeline));
    tv.rawXml = xml;
    parseTimelineXml(xml, &tv);

    // The XML mirrors membership between <mlSet> and <tskSet> regardless of task
    // kind, so derive the milestone flag from the actual task.
    QHash<int, const schedule::Task *> byUid;
    for (const schedule::Task &t : out->tasks)
        byUid.insert(t.uniqueId, &t);
    for (schedule::TimelineItem &it : tv.items)
        if (const schedule::Task *t = byUid.value(it.taskUid))
            it.milestone = t->milestone || t->durationMillis == 0;
}

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
        if (len % 2 != 0)
            ++o;
    }

    const QByteArray fixedMeta = cf.readStream({ kViewStorage, kCView, QStringLiteral("FixedMeta") });
    const QByteArray fixedData = cf.readStream({ kViewStorage, kCView, QStringLiteral("FixedData") });

    BkndVarData vd;
    if (!vd.parse(cf.readStream({ kViewStorage, kCView, QStringLiteral("VarMeta") }),
                  cf.readStream({ kViewStorage, kCView, QStringLiteral("Var2Data") })))
        return;

    readUsageView(cf, fixedMeta, fixedData, vd, kViewTypeGantt,
                  &out->ganttView);
    readUsageView(cf, fixedMeta, fixedData, vd, kViewTypeResourceUsage,
                  &out->resourceUsageView);
    readUsageView(cf, fixedMeta, fixedData, vd, kViewTypeTaskUsage,
                  &out->taskUsageView);
    readUsageView(cf, fixedMeta, fixedData, vd, kViewTypeTeamPlanner,
                  &out->teamPlannerView);

    const auto readSelection = [&cf](const QString &stream) {
        QStringList selection;
        const QJsonDocument document = QJsonDocument::fromJson(
            cf.readStream({kExtensionStorage, stream}));
        if (!document.isArray())
            return selection;
        for (const QJsonValue &value : document.array()) {
            if (value.isString() && !value.toString().isEmpty()
                && !selection.contains(value.toString()))
                selection.append(value.toString());
        }
        return selection;
    };
    out->taskUsageView.detailSelection = readSelection(kTaskUsageSelection);
    out->resourceUsageView.detailSelection = readSelection(kResourceUsageSelection);

    // The Timeline view: a "<TLViewData>" XML document (CV_iew var type 47 ==
    // Props9 item 574619695). Independent of the Gantt view below.
    readTimeline(fixedMeta, fixedData, vd, out);

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
    if (const PropsItem *bars = props.find(kKeyBarExceptions)) {
        // Kept whole as well as decoded: a save has to hand back the shapes and
        // bar text alongside the one field (the middle colour) we model.
        out->mppBarExceptions = bars->data;
        readBarExceptions(bars->data, out);
    }

    // readStyleData() only decodes each text category's fontBaseIndex; resolve it
    // against the font table so callers (e.g. the Gantt view's bar/task-detail text)
    // see the actual family/size the file specifies, not an empty/default font.
    for (schedule::TextStyle &style : out->viewStyles.text)
        hydrateFont(style, out->mppFontBases);

    for (schedule::Task &task : out->tasks) {
        hydrateFont(task.rowFormat, out->mppFontBases);
        for (auto it = task.cellFormats.begin(); it != task.cellFormats.end(); ++it)
            hydrateFont(it.value(), out->mppFontBases);
    }
}

void prepareFontBases(schedule::Project *project, const QByteArray &fallback)
{
    if (!project)
        return;
    if (project->mppFontBases.isEmpty())
        project->mppFontBases = fallback;
    if (fontBaseCount(project->mppFontBases) == 0)
        return;

    for (int i = 0; i < schedule::ViewStyles::TextCategoryCount; ++i) {
        schedule::TextStyle &style = project->viewStyles.text[i];
        const int index = ensureFontBase(project->mppFontBases, style);
        if (index >= 0)
            style.fontBaseIndex = index;
    }
    for (schedule::Task &task : project->tasks) {
        const int rowIndex = ensureFontBase(project->mppFontBases, task.rowFormat);
        if (rowIndex >= 0 && (!task.rowFormat.fontName.isEmpty() || task.rowFormat.fontSize > 0))
            task.rowFormat.fontBaseIndex = rowIndex;
        for (auto it = task.cellFormats.begin(); it != task.cellFormats.end(); ++it) {
            const int cellIndex = ensureFontBase(project->mppFontBases, it.value());
            if (cellIndex >= 0 && (!it->fontName.isEmpty() || it->fontSize > 0))
                it->fontBaseIndex = cellIndex;
        }
    }
}

bool wantsPatch(const schedule::Project &in)
{
    if (in.viewStyles.present || in.resourceUsageStyles.present
        || in.teamPlannerStyles.present || in.calendarStyles.present)
        return true;
    if (in.ganttView.modified || in.resourceUsageView.modified || in.taskUsageView.modified
        || in.teamPlannerView.modified)
        return true;
    if (in.timelineView.present && in.timelineView.modified)
        return true;
    if (!in.mppBarExceptions.isEmpty())
        return true;
    for (const schedule::Task &t : in.tasks)
        if (!t.rowFormat.isDefault() || !t.cellFormats.isEmpty()
            || t.barColor != schedule::TextStyle::kAutomatic)
            return true;
    return false;
}

bool wantsTablePatch(const schedule::Project &in)
{
    return in.ganttView.modified || in.resourceUsageView.modified || in.taskUsageView.modified
        || in.teamPlannerView.modified;
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

            if (in.ganttView.modified && in.ganttView.tableWidth > 0) {
                if (PropsItem *table = props.find(kKeyTableProperties)) {
                    if (table->data.size() >= 37)
                        wrU16(table->data, 35, quint16(qBound(0, in.ganttView.tableWidth, 65535)));
                }
            }

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

            const QByteArray barExceptions = buildBarExceptions(in);
            if (PropsItem *bars = props.find(kKeyBarExceptions)) {
                if (barExceptions.isEmpty()) {
                    for (int i = 0; i < props.items.size(); ++i)
                        if (props.items[i].key == kKeyBarExceptions) {
                            props.items.removeAt(i);
                            break;
                        }
                } else {
                    bars->data = barExceptions;
                    if (bars->flags == 0)
                        bars->flags = 1;
                }
            } else if (!barExceptions.isEmpty()) {
                PropsItem item;
                item.key = kKeyBarExceptions;
                item.flags = 1;
                item.data = barExceptions;
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

    // Timeline view: re-emit the "<TLViewData>" document into BOTH storage
    // locations byte-identically -- the CV_iew type-47 var record and the
    // type-6 Props9 item 574619695 (MS Project keeps the two in lock-step).
    if (in.timelineView.present && in.timelineView.modified) {
        const int tlUid = findViewUid(fixedMeta, fixedData, kViewTypeTimeline);
        const QByteArray newXml = serializeTimelineXml(in.timelineView);
        if (tlUid < 0 || newXml.isEmpty()) {
            qWarning("ViewFormat: project carries an edited Timeline but the template "
                     "has no type-16 view / no re-emittable XML; timeline edits dropped "
                     "(v1 preserve-existing).");
        } else if (newXml != in.timelineView.rawXml) {
            const auto recordForVarType = [&](int uid, quint16 type) -> int {
                for (int i = 0; i < records.size(); ++i)
                    if (records[i].uid == quint32(uid) && records[i].typeLow == type)
                        return i;
                return -1;
            };
            const int memRec = recordForVarType(tlUid, kTimelineXmlVarType);
            if (memRec >= 0)
                patched.insert(memRec, newXml);

            const int propsRec = recordForView(tlUid);
            if (propsRec >= 0) {
                Props9 props;
                const bool parsed = patched.contains(propsRec)
                    ? parseProps9(patched.value(propsRec), &props)
                    : parseRecordProps(propsRec, &props);
                if (parsed) {
                    if (PropsItem *xmlItem = props.find(kKeyTimelineXml)) {
                        xmlItem->data = newXml;
                        patched.insert(propsRec, buildProps9(props));
                    }
                }
            }
        }
    }

    // Native Usage-view timescale expansion and ordered detail fields.
    // Resource/Task Usage keep the authoritative counted field list in
    // STYLE_DATA and newer files duplicate it in an FF-padded VIEW_FIELDS item.
    const struct { quint16 type; const schedule::UsageViewSettings *settings; } kUsage[] = {
        { kViewTypeResourceUsage, &in.resourceUsageView },
        { kViewTypeTaskUsage, &in.taskUsageView },
        { kViewTypeTeamPlanner, &in.teamPlannerView },
    };
    for (const auto &v : kUsage) {
        if (!v.settings->modified)
            continue;
        const int uid = findViewUid(fixedMeta, fixedData, v.type);
        const int rec = uid >= 0 ? recordForView(uid) : -1;
        if (rec < 0)
            continue;
        Props9 props;
        const bool parsed = patched.contains(rec)
            ? parseProps9(patched.value(rec), &props)
            : parseRecordProps(rec, &props);
        if (!parsed)
            continue;
        bool changed = false;
        if (v.settings->tableWidth > 0) {
            if (PropsItem *table = props.find(kKeyTableProperties)) {
                if (table->data.size() >= 37) {
                    wrU16(table->data, 35, quint16(qBound(
                        0, v.settings->tableWidth, 65535)));
                    changed = true;
                }
            }
        }
        if (PropsItem *style = props.find(kKeyStyleData)) {
            if (style->data.size() > kTimescaleSizeOffset) {
                style->data[kTimescaleSizeOffset] = char(
                    qBound(25, v.settings->timescaleSize, 255));
                if (!v.settings->detailFields.isEmpty()
                    && style->data.size() > kUsageFieldCountOffset) {
                    const int oldCount = quint8(style->data.at(kUsageFieldCountOffset));
                    const int count = qMin(v.settings->detailFields.size(), 255);
                    if (kUsageFieldsOffset + qMax(oldCount, count) <= style->data.size()) {
                        for (int i = 0; i < qMax(oldCount, count); ++i)
                            style->data[kUsageFieldsOffset + i] = 0;
                        style->data[kUsageFieldCountOffset] = char(count);
                        for (int i = 0; i < count; ++i)
                            style->data[kUsageFieldsOffset + i] =
                                char(v.settings->detailFields.at(i));
                    }
                }
                changed = true;
            }
        }
        if (!v.settings->detailFields.isEmpty()) {
            PropsItem *fields = props.find(kKeyViewFields);
            if (fields) {
                fields->data.fill(char(0xFF));
                const int count = qMin(fields->data.size(), v.settings->detailFields.size());
                for (int i = 0; i < count; ++i)
                    fields->data[i] = char(v.settings->detailFields.at(i));
                changed = true;
            }
        }
        if (changed)
            patched.insert(rec, buildProps9(props));
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

bool patchTables(const CompoundFile &tpl, CompoundFile &out, const schedule::Project &in)
{
    if (!wantsTablePatch(in) || !tpl.hasStorage({ kViewStorage, kCTable }))
        return false;

    const QByteArray fixedData = tpl.readStream(
        { kViewStorage, kCTable, QStringLiteral("FixedData") });
    const QByteArray varMeta = tpl.readStream(
        { kViewStorage, kCTable, QStringLiteral("VarMeta") });
    QByteArray var2 = tpl.readStream(
        { kViewStorage, kCTable, QStringLiteral("Var2Data") });
    QVector<VarRecord> records;
    if (!parseVarMeta(varMeta, &records))
        return false;

    struct Target { const schedule::UsageViewSettings *settings; bool resource; };
    const Target targets[] = {
        { &in.ganttView, false }, { &in.resourceUsageView, true },
        { &in.taskUsageView, false }, { &in.teamPlannerView, true }
    };
    bool changed = false;
    for (const Target &target : targets) {
        if (!target.settings->modified || target.settings->tableName.isEmpty())
            continue;
        const QString wanted = normalizedPresentationName(target.settings->tableName);
        quint32 tableUid = 0;
        for (int offset = 0; offset + kTableRecordSize <= fixedData.size();
             offset += kTableRecordSize) {
            const QByteArray rec = fixedData.mid(offset, kTableRecordSize);
            if ((rdU16(rec, 108) == 1) != target.resource)
                continue;
            if (normalizedPresentationName(rdUtf16(rec, 4, 104)) == wanted) {
                tableUid = rdU32(rec, 0);
                break;
            }
        }
        if (tableUid == 0)
            continue;

        int recordIndex = -1;
        for (quint16 wantedType : { quint16(8), quint16(7), quint16(6) }) {
            for (int i = 0; i < records.size(); ++i) {
                if (records[i].uid == tableUid && records[i].typeLow == wantedType) {
                    recordIndex = i;
                    break;
                }
            }
            if (recordIndex >= 0)
                break;
        }
        if (recordIndex < 0)
            continue;
        const int blobOffset = int(records[recordIndex].offset);
        const int length = int(rdU32(var2, blobOffset));
        if (length < kTableColumnHeaderSize || blobOffset + 4 + length > var2.size())
            continue;
        const int count = int(rdU16(var2, blobOffset + 4 + 4)) + 1;
        for (int i = 0; i < count; ++i) {
            const int at = blobOffset + 4 + kTableColumnHeaderSize + i * kTableColumnSize;
            if (at + kTableColumnSize > blobOffset + 4 + length)
                break;
            const quint32 fieldId = rdU32(var2, at);
            for (const schedule::UsageTableColumn &column : target.settings->columns) {
                if (column.fieldId == fieldId) {
                    const char value = char(qBound(0, column.width, 255));
                    if (var2.at(at + 4) != value) {
                        var2[at + 4] = value;
                        changed = true;
                    }
                    break;
                }
            }
        }
    }

    if (!changed)
        return false;
    out.addStream({ kViewStorage, kCTable, QStringLiteral("VarMeta") }, varMeta);
    out.addStream({ kViewStorage, kCTable, QStringLiteral("Var2Data") }, var2);
    return true;
}

void writeExtensions(CompoundFile &out, const schedule::Project &in)
{
    const auto writeSelection = [&out](const QString &stream,
                                       const QStringList &details) {
        if (details.isEmpty())
            return;
        QJsonArray selection;
        for (const QString &detail : details)
            selection.append(detail);
        out.addStream({kExtensionStorage, stream},
                      QJsonDocument(selection).toJson(QJsonDocument::Compact));
    };
    writeSelection(kTaskUsageSelection, in.taskUsageView.detailSelection);
    writeSelection(kResourceUsageSelection, in.resourceUsageView.detailSelection);
}

} // namespace ViewFormat
