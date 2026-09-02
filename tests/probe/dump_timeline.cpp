// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Diagnostic (throwaway): reverse-engineer the Microsoft Project Timeline view
// stored in "   214/CV_iew". The Timeline is view type 16; its task membership
// is a Var2Data record of type 47 keyed by the timeline view uid, and its
// formatting is the view's type-6 (PROPERTIES / Props9) blob. Neither is
// decoded by the codec yet -- this probe dumps both so their layout can be
// worked out from a fixture matrix.
//
// usage:
//   dump_timeline <file.mpp>
//       Dump the timeline view record, its type-6 Props9 item list (decoding
//       every item whose key is not a known Gantt key -- most are UTF-16 text,
//       including the "<TLViewData ...>" XML document), and its type-47
//       membership blob, and report whether the two XML copies are identical.
//
//   dump_timeline --diff <a.mpp> <b.mpp>
//       Align the type-47 blobs and the type-6 Props9 blobs of two fixtures and
//       print every differing byte offset with its old/new value. This diff is
//       the primary RE lever for the field layout.

#include "ole/compoundfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtEndian>
#include <cstdio>

namespace {

constexpr quint16 kViewTypeTimeline = 16;
constexpr quint16 kViewPropsType = 6;      // PROPERTIES var-record type
constexpr quint16 kMembershipType = 47;    // timeline task-membership var-record type
constexpr int kViewRecordSize = 138;       // CV_iew FixedData block size

// Known Gantt/Usage Props9 keys -- items with these keys are already understood
// by the codec, so the probe only hexdumps the *other* items.
constexpr quint32 kKeyStyleData = 574619656u;         // STYLE_DATA
constexpr quint32 kKeyColumnProperties = 574619660u;  // COLUMN_PROPERTIES
constexpr quint32 kKeyTableName = 574619658u;
constexpr quint32 kKeyTableProperties = 574619655u;
constexpr quint32 kKeyViewFields = 574619708u;
constexpr quint32 kKeyFontBases = 0x03400000u;

quint32 u32(const QByteArray &d, int o)
{
    return (o < 0 || o + 4 > d.size())
        ? 0
        : qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(d.constData()) + o);
}
quint16 u16(const QByteArray &d, int o)
{
    return (o < 0 || o + 2 > d.size())
        ? 0
        : qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(d.constData()) + o);
}

QString utf16z(const QByteArray &d, int o, int maxBytes)
{
    QString s;
    for (int b = 0; b + 1 < maxBytes; b += 2) {
        const ushort ch = u16(d, o + b);
        if (ch == 0)
            break;
        s.append(QChar(ch));
    }
    return s;
}

struct CView {
    QByteArray fixedMeta, fixedData, varMeta, var2;
    bool ok = false;
};

CView readCView(const QString &path)
{
    CView v;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("cannot open %s\n", qPrintable(path));
        return v;
    }
    CompoundFile cf;
    if (!cf.openFromData(f.readAll())) {
        std::printf("CFB parse failed for %s\n", qPrintable(path));
        return v;
    }
    const QStringList base = { QStringLiteral("   214"), QStringLiteral("CV_iew") };
    v.fixedMeta = cf.readStream(base + QStringList{ QStringLiteral("FixedMeta") });
    v.fixedData = cf.readStream(base + QStringList{ QStringLiteral("FixedData") });
    v.varMeta = cf.readStream(base + QStringList{ QStringLiteral("VarMeta") });
    v.var2 = cf.readStream(base + QStringList{ QStringLiteral("Var2Data") });
    v.ok = !v.fixedData.isEmpty() && !v.varMeta.isEmpty();
    return v;
}

// Walk FixedMeta 10-byte items (offset u16@+4 into FixedData) and return the uid
// (u32@0) of the first non-split record whose viewType (u16@112) is 16.
int timelineViewUid(const CView &v, int *recordOffset = nullptr)
{
    const int items = v.fixedMeta.size() >= 16 ? int(u32(v.fixedMeta, 8)) : 0;
    int lastOffset = -1;
    for (int i = 0; i < items; ++i) {
        const int mo = 16 + i * 10;
        if (mo + 10 > v.fixedMeta.size())
            break;
        const int offset = u16(v.fixedMeta, mo + 4);
        if (offset <= lastOffset)
            continue;
        lastOffset = offset;
        if (offset + kViewRecordSize > v.fixedData.size())
            continue;
        if (u16(v.fixedData, offset + 110) != 0)   // splitViewFlag
            continue;
        if (u16(v.fixedData, offset + 112) == kViewTypeTimeline) {
            if (recordOffset)
                *recordOffset = offset;
            return int(u32(v.fixedData, offset + 0));
        }
    }
    return -1;
}

// Return the raw payload (without the leading u32 length) of the Var2Data record
// for (uid, type), or an empty array.
QByteArray varBlob(const CView &v, quint32 uid, quint16 type)
{
    if (v.varMeta.size() < 24 || u32(v.varMeta, 0) != 0xFADFADBAu)
        return {};
    const int count = int(u32(v.varMeta, 8));
    for (int i = 0; i < count; ++i) {
        const int o = 24 + i * 12;
        if (o + 12 > v.varMeta.size())
            break;
        if (u32(v.varMeta, o) == uid && u16(v.varMeta, o + 8) == type) {
            const int off = int(u32(v.varMeta, o + 4));
            const int len = int(u32(v.var2, off));
            if (off + 4 + len > v.var2.size())
                return {};
            return v.var2.mid(off + 4, len);
        }
    }
    return {};
}

void hexdump(const QByteArray &b, const char *indent = "  ");

// Many Timeline sub-blobs are UTF-16LE text (Microsoft's "<TLViewData ...>" XML
// document, view/group names, ...). Detect that and print it verbatim instead
// of a 10 KB hexdump.
bool looksLikeUtf16Text(const QByteArray &b)
{
    if (b.size() < 8 || b.size() % 2 != 0)
        return false;
    int printable = 0, total = 0;
    for (int i = 0; i + 1 < b.size() && total < 64; i += 2, ++total) {
        if (b.at(i + 1) != 0)
            return false;   // high byte must be 0 for BMP ASCII-ish text
        const uchar lo = uchar(b.at(i));
        if (lo == 0 || (lo >= 0x20 && lo < 0x7f) || lo == 0x09 || lo == 0x0a || lo == 0x0d)
            ++printable;
    }
    return printable == total;
}

QString decodeUtf16(const QByteArray &b)
{
    QString s;
    for (int i = 0; i + 1 < b.size(); i += 2) {
        const ushort ch = u16(b, i);
        if (ch == 0)
            continue;
        s.append(QChar(ch));
    }
    return s;
}

// Pretty-print the TLViewData XML with one element per line so attribute changes
// between fixtures are diffable by eye.
void printXml(const QString &xml, const char *indent)
{
    QString cur;
    int depth = 0;
    for (int i = 0; i < xml.size(); ++i) {
        const QChar c = xml.at(i);
        cur.append(c);
        if (c == QLatin1Char('>')) {
            const QString t = cur.trimmed();
            const bool closing = t.startsWith(QStringLiteral("</"));
            const bool selfClose = t.endsWith(QStringLiteral("/>"));
            if (closing)
                depth = qMax(0, depth - 1);
            std::printf("%s%*s%s\n", indent, depth * 2, "", qPrintable(t));
            if (!closing && !selfClose && !t.startsWith(QStringLiteral("<?")))
                ++depth;
            cur.clear();
        }
    }
    if (!cur.trimmed().isEmpty())
        std::printf("%s%s\n", indent, qPrintable(cur.trimmed()));
}

void dumpBlob(const QByteArray &b, const char *indent)
{
    if (looksLikeUtf16Text(b)) {
        const QString s = decodeUtf16(b);
        if (s.contains(QLatin1Char('<')) && s.contains(QLatin1Char('>')))
            printXml(s, indent);
        else
            std::printf("%s\"%s\"\n", indent, qPrintable(s));
        return;
    }
    hexdump(b, indent);
}

void hexdump(const QByteArray &b, const char *indent)
{
    for (int i = 0; i < b.size(); i += 16) {
        std::printf("%s%04x  ", indent, i);
        for (int j = 0; j < 16; ++j) {
            if (i + j < b.size())
                std::printf("%02x ", uchar(b.at(i + j)));
            else
                std::printf("   ");
            if (j == 7)
                std::printf(" ");
        }
        std::printf(" |");
        for (int j = 0; j < 16 && i + j < b.size(); ++j) {
            const uchar c = uchar(b.at(i + j));
            std::printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        std::printf("|\n");
    }
}

// Parse a Props9 block: 16-byte header (item count u16@12), then items
// [u32 size][u32 key][u32 flags][data], each padded to a 2-byte boundary.
struct PropItem {
    quint32 key = 0;
    quint32 flags = 0;
    int dataOffset = 0;   // offset into the Props9 block of the item payload
    int size = 0;
};

QVector<PropItem> parseProps9(const QByteArray &props)
{
    QVector<PropItem> out;
    if (props.size() < 16)
        return out;
    const int count = u16(props, 12);
    int po = 16;
    for (int i = 0; i < count; ++i) {
        if (po + 12 > props.size())
            break;
        PropItem it;
        it.size = int(u32(props, po));
        it.key = u32(props, po + 4);
        it.flags = u32(props, po + 8);
        po += 12;
        it.dataOffset = po;
        if (po + it.size > props.size())
            break;
        out.append(it);
        po += it.size;
        if (it.size % 2 != 0)
            ++po;
    }
    return out;
}

bool knownGanttKey(quint32 key)
{
    return key == kKeyStyleData || key == kKeyColumnProperties || key == kKeyTableName
        || key == kKeyTableProperties || key == kKeyViewFields || key == kKeyFontBases;
}

int dumpOne(const QString &path)
{
    const CView v = readCView(path);
    if (!v.ok) {
        std::printf("no usable CV_iew streams\n");
        return 2;
    }
    std::printf("== %s ==\n", qPrintable(path));
    std::printf("FixedMeta=%d FixedData=%d VarMeta=%d Var2Data=%d\n",
                int(v.fixedMeta.size()), int(v.fixedData.size()),
                int(v.varMeta.size()), int(v.var2.size()));

    int recOff = -1;
    const int uid = timelineViewUid(v, &recOff);
    if (uid < 0) {
        std::printf("no Timeline view (type %u) in this file\n", kViewTypeTimeline);
        return 1;
    }
    std::printf("\n-- Timeline view record (uid=%d, FixedData offset=%d) --\n", uid, recOff);
    std::printf("name=\"%s\"\n", qPrintable(utf16z(v.fixedData, recOff + 4, 104)));
    std::printf("bytes 100..138 (past the name; splitFlag@110, viewType@112):\n");
    hexdump(v.fixedData.mid(recOff + 100, 38));

    // ---- type-6 PROPERTIES ----
    const QByteArray props = varBlob(v, quint32(uid), kViewPropsType);
    QByteArray propsTlView;   // the embedded <TLViewData> Props9 item, if any
    std::printf("\n-- type-6 PROPERTIES (Props9), %d bytes --\n", int(props.size()));
    if (props.isEmpty()) {
        std::printf("(none)\n");
    } else {
        std::printf("header:");
        for (int b = 0; b < 16 && b < props.size(); ++b)
            std::printf(" %02x", uchar(props.at(b)));
        std::printf("\n");
        const QVector<PropItem> items = parseProps9(props);
        std::printf("item count (u16@12)=%d, parsed=%d\n", u16(props, 12), int(items.size()));
        for (int i = 0; i < items.size(); ++i) {
            const PropItem &it = items.at(i);
            std::printf("  item %d: key=%u size=%d flags=0x%x %s\n", i, it.key, it.size,
                        it.flags, knownGanttKey(it.key) ? "(known Gantt key)" : "<-- UNKNOWN");
            if (!knownGanttKey(it.key)) {
                const QByteArray blob = props.mid(it.dataOffset, it.size);
                dumpBlob(blob, "    ");
                if (looksLikeUtf16Text(blob) && decodeUtf16(blob).contains(QStringLiteral("TLViewData")))
                    propsTlView = blob;
            }
        }
    }

    // ---- type-47 membership ----
    const QByteArray mem = varBlob(v, quint32(uid), kMembershipType);
    std::printf("\n-- type-47 membership blob, %d bytes --\n", int(mem.size()));
    if (mem.isEmpty())
        std::printf("(none)\n");
    else
        dumpBlob(mem, "  ");

    if (!mem.isEmpty() || !propsTlView.isEmpty()) {
        std::printf("\n-- type-47 vs Props9 <TLViewData> item --\n");
        if (mem.isEmpty())
            std::printf("  type-47 absent; XML lives only in Props9\n");
        else if (propsTlView.isEmpty())
            std::printf("  Props9 has no <TLViewData> item; XML lives only in type-47\n");
        else
            std::printf("  %s (type-47 %d bytes, Props9 item %d bytes)\n",
                        mem == propsTlView ? "IDENTICAL" : "*** DIFFER ***",
                        int(mem.size()), int(propsTlView.size()));
    }
    return 0;
}

void diffBlobs(const char *label, const QByteArray &a, const QByteArray &b)
{
    std::printf("\n-- diff: %s --\n", label);
    if (a.isEmpty() && b.isEmpty()) {
        std::printf("both empty\n");
        return;
    }
    std::printf("size a=%d b=%d%s\n", int(a.size()), int(b.size()),
                a.size() == b.size() ? "" : "  *** LENGTH DIFFERS ***");
    const int n = qMin(a.size(), b.size());
    int diffs = 0;
    for (int o = 0; o < n; ++o) {
        if (a.at(o) != b.at(o)) {
            std::printf("  @%4d (0x%03x): %02x -> %02x\n", o, o,
                        uchar(a.at(o)), uchar(b.at(o)));
            ++diffs;
        }
    }
    if (diffs == 0 && a.size() == b.size())
        std::printf("  identical\n");
    else
        std::printf("  %d differing byte(s) in the common prefix\n", diffs);
}

int runDiff(const QString &pathA, const QString &pathB)
{
    const CView a = readCView(pathA);
    const CView b = readCView(pathB);
    if (!a.ok || !b.ok) {
        std::printf("could not read both files\n");
        return 2;
    }
    const int ua = timelineViewUid(a);
    const int ub = timelineViewUid(b);
    std::printf("timeline uid a=%d b=%d\n", ua, ub);
    if (ua < 0 || ub < 0) {
        std::printf("both files must contain a Timeline view\n");
        return 1;
    }
    diffBlobs("type-47 membership",
              varBlob(a, quint32(ua), kMembershipType),
              varBlob(b, quint32(ub), kMembershipType));
    diffBlobs("type-6 PROPERTIES (raw)",
              varBlob(a, quint32(ua), kViewPropsType),
              varBlob(b, quint32(ub), kViewPropsType));

    // Props9 item-aware view: line up items by key and report per-item size/flag
    // changes so a structural edit is easy to spot.
    const QVector<PropItem> ia = parseProps9(varBlob(a, quint32(ua), kViewPropsType));
    const QVector<PropItem> ib = parseProps9(varBlob(b, quint32(ub), kViewPropsType));
    std::printf("\n-- diff: type-6 Props9 items --\n");
    std::printf("  a: %d items, b: %d items\n", int(ia.size()), int(ib.size()));
    for (const PropItem &x : ia) {
        bool found = false;
        for (const PropItem &y : ib) {
            if (y.key == x.key) {
                found = true;
                if (y.size != x.size || y.flags != x.flags)
                    std::printf("  key=%u: size %d->%d flags 0x%x->0x%x\n",
                                x.key, x.size, y.size, x.flags, y.flags);
                break;
            }
        }
        if (!found)
            std::printf("  key=%u present in a, absent in b\n", x.key);
    }
    for (const PropItem &y : ib) {
        bool found = false;
        for (const PropItem &x : ia)
            if (x.key == y.key) { found = true; break; }
        if (!found)
            std::printf("  key=%u present in b, absent in a\n", y.key);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--diff")) {
        if (argc < 4) {
            std::printf("usage: dump_timeline --diff <a.mpp> <b.mpp>\n");
            return 2;
        }
        return runDiff(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    }
    if (argc < 2) {
        std::printf("usage: dump_timeline <file.mpp>\n"
                    "       dump_timeline --diff <a.mpp> <b.mpp>\n");
        return 2;
    }
    return dumpOne(QString::fromLocal8Bit(argv[1]));
}
