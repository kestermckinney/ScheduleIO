// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "barstylecodec.h"

#include "model/viewstyles.h"

#include <QtEndian>

namespace {

quint8 rd8(const QByteArray &b, int o)
{
    return (o < 0 || o >= b.size()) ? 0 : quint8(b.at(o));
}
quint16 rd16(const QByteArray &b, int o)
{
    return (o < 0 || o + 2 > b.size())
        ? 0 : qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(b.constData()) + o);
}
quint32 rd32(const QByteArray &b, int o)
{
    return (o < 0 || o + 4 > b.size())
        ? 0 : qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData()) + o);
}
quint64 rd64(const QByteArray &b, int o)
{
    return (o < 0 || o + 8 > b.size())
        ? 0 : qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(b.constData()) + o);
}

void wr8(QByteArray &b, int o, quint8 v)
{
    if (o >= 0 && o < b.size())
        b[o] = char(v);
}
void wr16(QByteArray &b, int o, quint16 v)
{
    if (o >= 0 && o + 2 <= b.size())
        qToLittleEndian(v, reinterpret_cast<uchar *>(b.data()) + o);
}
void wr32(QByteArray &b, int o, quint32 v)
{
    if (o >= 0 && o + 4 <= b.size())
        qToLittleEndian(v, reinterpret_cast<uchar *>(b.data()) + o);
}
void wr64(QByteArray &b, int o, quint64 v)
{
    if (o >= 0 && o + 8 <= b.size())
        qToLittleEndian(v, reinterpret_cast<uchar *>(b.data()) + o);
}

// Colour is r,g,b + a flag byte: nonzero flag = "Automatic". Same encoding as
// viewformat.cpp's rdColor/wrColor.
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

// --- 195-byte record field offsets (see DECODING_NOTES.md) ---
constexpr int kMiddleShape = 0;
constexpr int kMiddlePattern = 1;
constexpr int kMiddleColor = 2;
constexpr int kMiddleFlag = 14;
constexpr int kStartShape = 15;
constexpr int kStartColor = 16;
constexpr int kEndShape = 28;
constexpr int kEndColor = 29;
constexpr int kFromField = 41;
constexpr int kToField = 45;
constexpr int kShowFor = 49;
constexpr int kShowForNot = 57;
constexpr int kRow = 65;
constexpr int kBarText0 = 67;   // then +71, +75, +79, +83
constexpr int kBarTextStride = 4;
constexpr int kFlag87 = 87;
constexpr int kStyleId = 89;
constexpr int kName = 91;
constexpr int kNameMaxBytes = BarStyleCodec::kRecordSize - kName;   // 104

} // namespace

namespace BarStyleCodec {

schedule::ViewBarStyle readRecord(const QByteArray &d, int off)
{
    schedule::ViewBarStyle b;
    if (off < 0 || off + kRecordSize > d.size())
        return b;

    b.middleShape = rd8(d, off + kMiddleShape);
    b.middlePattern = rd8(d, off + kMiddlePattern);
    b.middleColor = rdColor(d, off + kMiddleColor);
    b.middleFlag = rd8(d, off + kMiddleFlag);
    b.startShape = rd8(d, off + kStartShape);
    b.startColor = rdColor(d, off + kStartColor);
    b.endShape = rd8(d, off + kEndShape);
    b.endColor = rdColor(d, off + kEndColor);
    b.fromField = qint32(rd32(d, off + kFromField));
    b.toField = qint32(rd32(d, off + kToField));
    b.showFor = rd64(d, off + kShowFor);
    b.showForNot = rd64(d, off + kShowForNot);
    // The file stores the row 0-based; the model (and MS Project's dialog) is
    // 1-based, 1..4.
    b.row = qBound(1, int(rd16(d, off + kRow)) + 1, 4);
    for (int i = 0; i < 5; ++i)
        b.barText[i] = qint32(rd32(d, off + kBarText0 + i * kBarTextStride));
    b.flag87 = rd16(d, off + kFlag87);
    b.styleId = rd16(d, off + kStyleId);

    QString name;
    for (int i = off + kName; i + 1 < d.size() && i < off + kName + kNameMaxBytes; i += 2) {
        const ushort ch = rd16(d, i);
        if (ch == 0)
            break;
        name.append(QChar(ch));
    }
    b.name = name;
    return b;
}

void writeRecord(QByteArray &d, int off, const schedule::ViewBarStyle &b)
{
    if (off < 0 || off + kRecordSize > d.size())
        return;

    wr8(d, off + kMiddleShape, b.middleShape);
    wr8(d, off + kMiddlePattern, b.middlePattern);
    wrColor(d, off + kMiddleColor, b.middleColor);
    wr8(d, off + kMiddleFlag, b.middleFlag);
    wr8(d, off + kStartShape, b.startShape);
    wrColor(d, off + kStartColor, b.startColor);
    wr8(d, off + kEndShape, b.endShape);
    wrColor(d, off + kEndColor, b.endColor);
    wr32(d, off + kFromField, quint32(b.fromField));
    wr32(d, off + kToField, quint32(b.toField));
    wr64(d, off + kShowFor, b.showFor);
    wr64(d, off + kShowForNot, b.showForNot);
    wr16(d, off + kRow, quint16(qBound(1, b.row, 4) - 1));
    for (int i = 0; i < 5; ++i)
        wr32(d, off + kBarText0 + i * kBarTextStride, quint32(b.barText[i]));
    wr16(d, off + kFlag87, b.flag87);
    wr16(d, off + kStyleId, b.styleId);

    // Name: UTF-16LE + a NUL terminator; the rest of the fixed 104-byte field is
    // left as-is (readRecord stops at the NUL, and MS Project pads it with 0xFF
    // on styles it authors -- preserving those bytes keeps the round-trip exact).
    const int chars = qMin(b.name.size(), (kNameMaxBytes - 2) / 2);
    for (int i = 0; i < chars; ++i)
        wr16(d, off + kName + i * 2, b.name.at(i).unicode());
    wr16(d, off + kName + chars * 2, 0);
}

} // namespace BarStyleCodec
