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

constexpr quint16 kViewPropsType = 6;   // GanttChartView14.PROPERTIES
constexpr quint16 kViewTypeGantt = 1;   // ViewType.GANTT_CHART
constexpr int kViewRecordSize = 138;    // CV_iew FixedData block size

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

// The exceptional-style record set written per formatted task: the standard
// Entry-table columns, so the whole visible row picks up the formatting.
// Values are MPXJ MPPTaskField FIELD_ARRAY indices; the full field id is
// 0x0B400000 | index.
constexpr quint16 kRowFields[] = { 23 /*ID*/, 14 /*NAME*/, 29 /*DURATION*/,
                                   35 /*START*/, 36 /*FINISH*/, 47 /*PREDECESSORS*/,
                                   49 /*RESOURCE_NAMES*/, 32 /*PERCENT_COMPLETE*/,
                                   0 /*WORK*/ };
constexpr quint16 kFieldName = 14;
constexpr quint32 kTaskFieldBase = 0x0B400000u;

// Change-flag bits of a 44-byte exceptional style record (MPXJ TableFontStyle).
constexpr quint16 kChgBold = 0x01, kChgUnderline = 0x02, kChgItalic = 0x04,
                  kChgColor = 0x08, kChgBackColor = 0x40, kChgBackPattern = 0x80;

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

// The Gantt Chart view's id in a CV_iew storage, or -1. Mirrors MPXJ
// MPP14Reader.processViewData: meta item u16@4 is the record's offset into the
// 138-byte-block FixedData; splitViewFlag u16@110 must be 0, type u16@112 == 1.
int findGanttViewUid(const QByteArray &fixedMeta, const QByteArray &fixedData)
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
        if (rdU16(rec, 112) == kViewTypeGantt)
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
    return b;
}

// ---- STYLE_DATA <-> ViewStyles ---------------------------------------------------

schedule::TextStyle readTextStyle(const QByteArray &d, int o)
{
    schedule::TextStyle s;
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

// ---- COLUMN_PROPERTIES (per-task exceptional styles) ----------------------------

void readColumnProperties(const QByteArray &d, schedule::Project *out)
{
    QHash<int, int> rowByUid;
    for (int i = 0; i < out->tasks.size(); ++i)
        rowByUid.insert(out->tasks[i].uniqueId, i);

    // One record per (uid, field); collapse to a row-level style, preferring the
    // Name column's record as the representative when a row has several.
    QHash<int, schedule::TextStyle> byUid;
    QHash<int, bool> haveName;
    const int count = d.size() / 44;
    for (int i = 0; i < count; ++i) {
        const int o = i * 44;
        const int uid = int(rdU32(d, o));
        if (!rowByUid.contains(uid))
            continue;
        const quint16 fieldIndex = quint16(rdU32(d, o + 4) & 0xFFFF);
        if (haveName.value(uid, false) && fieldIndex != kFieldName)
            continue;

        const int bits = uchar(d.at(o + 11));
        const quint16 change = rdU16(d, o + 40);
        schedule::TextStyle s;
        s.bold = (change & kChgBold) && (bits & 0x01);
        s.italic = (change & kChgItalic) && (bits & 0x02);
        s.underline = (change & kChgUnderline) && (bits & 0x04);
        s.color = (change & kChgColor) ? rdColor(d, o + 12)
                                       : schedule::TextStyle::kAutomatic;
        s.backColor = (change & kChgBackColor) ? rdColor(d, o + 24)
                                               : schedule::TextStyle::kAutomatic;
        s.backPattern = (change & kChgBackPattern) ? rdU16(d, o + 36) : 0;
        byUid.insert(uid, s);
        if (fieldIndex == kFieldName)
            haveName.insert(uid, true);
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
        for (quint16 field : kRowFields) {
            QByteArray rec(44, '\0');
            wrU32(rec, 0, quint32(t.uniqueId));
            wrU32(rec, 4, kTaskFieldBase | field);
            rec[8] = 0;   // font base 0; the font-changed flag stays off
            rec[11] = char((s.bold ? 0x01 : 0) | (s.italic ? 0x02 : 0)
                           | (s.underline ? 0x04 : 0)
                           | (s.strikethrough ? 0x08 : 0));
            wrColor(rec, 12, s.color);
            wrColor(rec, 24, s.backColor);
            // Written verbatim: callers set a solid pattern (1) alongside an
            // explicit background colour or nothing shows in MS Project.
            wrU16(rec, 36, quint16(s.backPattern));
            wrU16(rec, 40, quint16(kChgBold | kChgUnderline | kChgItalic
                                   | kChgColor | kChgBackColor | kChgBackPattern));
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

    const int viewUid = findGanttViewUid(
        cf.readStream({ kViewStorage, kCView, QStringLiteral("FixedMeta") }),
        cf.readStream({ kViewStorage, kCView, QStringLiteral("FixedData") }));
    if (viewUid < 0)
        return;

    BkndVarData vd;
    if (!vd.parse(cf.readStream({ kViewStorage, kCView, QStringLiteral("VarMeta") }),
                  cf.readStream({ kViewStorage, kCView, QStringLiteral("Var2Data") })))
        return;

    Props9 props;
    if (!parseProps9(vd.blobFor(quint32(viewUid), kViewPropsType), &props))
        return;

    if (const PropsItem *style = props.find(kKeyStyleData))
        readStyleData(style->data, &out->viewStyles);
    if (const PropsItem *cols = props.find(kKeyColumnProperties))
        readColumnProperties(cols->data, out);
}

bool wantsPatch(const schedule::Project &in)
{
    if (in.viewStyles.present)
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

    const QByteArray varMeta = tpl.readStream({ kViewStorage, kCView, QStringLiteral("VarMeta") });
    const QByteArray var2 = tpl.readStream({ kViewStorage, kCView, QStringLiteral("Var2Data") });
    const int viewUid = findGanttViewUid(
        tpl.readStream({ kViewStorage, kCView, QStringLiteral("FixedMeta") }),
        tpl.readStream({ kViewStorage, kCView, QStringLiteral("FixedData") }));
    if (viewUid < 0)
        return false;

    QVector<VarRecord> records;
    if (!parseVarMeta(varMeta, &records))
        return false;

    // Patch the Gantt view's Props9 blob.
    int propsRecord = -1;
    for (int i = 0; i < records.size(); ++i)
        if (records[i].uid == quint32(viewUid) && records[i].typeLow == kViewPropsType) {
            propsRecord = i;
            break;
        }
    if (propsRecord < 0)
        return false;

    const quint32 propsOffset = records[propsRecord].offset;
    const int propsLen = int(rdU32(var2, int(propsOffset)));
    if (propsLen <= 0 || int(propsOffset) + 4 + propsLen > var2.size())
        return false;

    Props9 props;
    if (!parseProps9(var2.mid(int(propsOffset) + 4, propsLen), &props))
        return false;

    if (in.viewStyles.present) {
        if (PropsItem *style = props.find(kKeyStyleData))
            patchStyleData(style->data, in.viewStyles);
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
        }
    } else if (!colProps.isEmpty()) {
        PropsItem item;
        item.key = kKeyColumnProperties;
        item.data = colProps;
        props.items.append(item);
    }

    const QByteArray newProps = buildProps9(props);

    // Rebuild Var2Data with every blob verbatim (in record order) except the
    // patched props blob, recomputing offsets; then rebuild VarMeta to match.
    QByteArray newVar2;
    QByteArray newMeta = varMeta.left(24);
    for (const VarRecord &r : records) {
        const int oldOff = int(r.offset);
        const int len = int(rdU32(var2, oldOff));
        QByteArray blob;
        if (&r == &records[propsRecord])
            blob = newProps;
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
