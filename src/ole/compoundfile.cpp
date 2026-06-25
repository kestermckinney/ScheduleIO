// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "ole/compoundfile.h"

#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <functional>

namespace {

constexpr quint32 kEndOfChain = 0xFFFFFFFE;
constexpr quint32 kFreeSect   = 0xFFFFFFFF;
constexpr quint32 kFatSect    = 0xFFFFFFFD;
constexpr quint32 kNoStream   = 0xFFFFFFFF;
constexpr quint32 kMaxRegSect = 0xFFFFFFFA;

constexpr int  kSectorSize     = 512;
constexpr int  kMiniSectorSize = 64;
constexpr int  kDirEntrySize   = 128;
constexpr quint32 kMiniCutoff  = 4096;
constexpr int  kHeaderDifatCount = 109;

const char kSignature[8] = { '\xd0', '\xcf', '\x11', '\xe0',
                             '\xa1', '\xb1', '\x1a', '\xe1' };

quint16 rdU16(const QByteArray &d, int off)
{ return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(d.constData() + off)); }
quint32 rdU32(const QByteArray &d, int off)
{ return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(d.constData() + off)); }
quint64 rdU64(const QByteArray &d, int off)
{ return qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(d.constData() + off)); }

void wrU16(QByteArray &d, int off, quint16 v)
{ qToLittleEndian<quint16>(v, reinterpret_cast<uchar *>(d.data() + off)); }
void wrU32(QByteArray &d, int off, quint32 v)
{ qToLittleEndian<quint32>(v, reinterpret_cast<uchar *>(d.data() + off)); }
void wrU64(QByteArray &d, int off, quint64 v)
{ qToLittleEndian<quint64>(v, reinterpret_cast<uchar *>(d.data() + off)); }

// [MS-CFB] directory name ordering: shorter names first, then case-insensitive
// (uppercased) UTF-16 code-unit comparison.
bool cfbNameLess(const QString &a, const QString &b)
{
    if (a.size() != b.size())
        return a.size() < b.size();
    const QString ua = a.toUpper();
    const QString ub = b.toUpper();
    return ua < ub;
}

} // namespace

bool CompoundFile::looksLikeCompoundFile(const QByteArray &bytes)
{
    return bytes.size() >= 8 && std::memcmp(bytes.constData(), kSignature, 8) == 0;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

bool CompoundFile::openFromData(const QByteArray &bytes)
{
    m_nodes.clear();
    m_root = -1;
    m_valid = false;
    m_error.clear();

    auto fail = [&](const QString &msg) { m_error = msg; m_valid = false; return false; };

    if (bytes.size() < kSectorSize)
        return fail(QStringLiteral("file shorter than a CFB header"));
    if (!looksLikeCompoundFile(bytes))
        return fail(QStringLiteral("not a compound file (bad signature)"));

    const quint16 sectorShift = rdU16(bytes, 0x1E);
    if (sectorShift != 9)   // only version-3 (512-byte) sectors supported
        return fail(QStringLiteral("unsupported sector shift %1").arg(sectorShift));

    const quint32 numFatSectors  = rdU32(bytes, 0x2C);
    const quint32 firstDirSector = rdU32(bytes, 0x30);
    const quint32 firstMiniFat   = rdU32(bytes, 0x3C);
    const quint32 numMiniFat     = rdU32(bytes, 0x40);
    const quint32 firstDifat     = rdU32(bytes, 0x44);
    const quint32 numDifat       = rdU32(bytes, 0x48);

    const int totalSectors = (bytes.size() - kSectorSize) / kSectorSize;
    auto sectorOffset = [](quint32 sector) { return kSectorSize + static_cast<int>(sector) * kSectorSize; };
    auto sectorValid  = [&](quint32 s) { return s <= kMaxRegSect && static_cast<int>(s) < totalSectors; };

    // Gather FAT sector locations: 109 from the header DIFAT, then any chained
    // DIFAT sectors.
    std::vector<quint32> fatSectors;
    for (int i = 0; i < kHeaderDifatCount && fatSectors.size() < numFatSectors; ++i) {
        const quint32 s = rdU32(bytes, 0x4C + i * 4);
        if (s == kFreeSect)
            break;
        if (!sectorValid(s))
            return fail(QStringLiteral("bad FAT sector in header DIFAT"));
        fatSectors.push_back(s);
    }
    quint32 difatSector = firstDifat;
    quint32 difatGuard = 0;
    while (difatSector != kEndOfChain && difatSector != kFreeSect
           && fatSectors.size() < numFatSectors) {
        if (!sectorValid(difatSector) || difatGuard++ > numDifat + 1)
            return fail(QStringLiteral("bad DIFAT chain"));
        const int base = sectorOffset(difatSector);
        for (int i = 0; i < (kSectorSize / 4) - 1 && fatSectors.size() < numFatSectors; ++i) {
            const quint32 s = rdU32(bytes, base + i * 4);
            if (s == kFreeSect)
                break;
            if (!sectorValid(s))
                return fail(QStringLiteral("bad FAT sector in DIFAT chain"));
            fatSectors.push_back(s);
        }
        difatSector = rdU32(bytes, base + kSectorSize - 4);
    }

    // Materialise the FAT as one big array.
    std::vector<quint32> fat;
    fat.reserve(fatSectors.size() * (kSectorSize / 4));
    for (quint32 fs : fatSectors) {
        const int base = sectorOffset(fs);
        for (int i = 0; i < kSectorSize / 4; ++i)
            fat.push_back(rdU32(bytes, base + i * 4));
    }

    // Walk a FAT chain, returning the concatenated full-sector bytes.
    auto readChain = [&](quint32 start) -> QByteArray {
        QByteArray out;
        quint32 s = start;
        std::size_t guard = 0;
        while (s != kEndOfChain && s != kFreeSect) {
            if (!sectorValid(s) || s >= fat.size() || guard++ > fat.size())
                return QByteArray();   // corrupt / cyclic chain
            out.append(bytes.constData() + sectorOffset(s), kSectorSize);
            s = fat[s];
        }
        return out;
    };

    // Read the directory stream and parse 128-byte entries.
    const QByteArray dirBytes = readChain(firstDirSector);
    if (dirBytes.isEmpty())
        return fail(QStringLiteral("empty or unreadable directory"));

    struct RawEntry {
        QString name;
        quint8 type = 0;
        quint32 left = kNoStream, right = kNoStream, child = kNoStream;
        quint32 start = 0;
        quint64 size = 0;
    };
    std::vector<RawEntry> raw;
    const int entryCount = dirBytes.size() / kDirEntrySize;
    for (int i = 0; i < entryCount; ++i) {
        const int o = i * kDirEntrySize;
        RawEntry e;
        e.type = static_cast<quint8>(dirBytes.at(o + 0x42));
        quint16 nameLen = rdU16(dirBytes, o + 0x40);
        if (nameLen > 64)
            nameLen = 64;
        if (nameLen >= 2)
            e.name = QString::fromUtf16(
                reinterpret_cast<const char16_t *>(dirBytes.constData() + o),
                (nameLen / 2) - 1);
        e.left  = rdU32(dirBytes, o + 0x44);
        e.right = rdU32(dirBytes, o + 0x48);
        e.child = rdU32(dirBytes, o + 0x4C);
        e.start = rdU32(dirBytes, o + 0x74);
        e.size  = rdU64(dirBytes, o + 0x78);
        raw.push_back(e);
    }
    if (raw.empty() || raw[0].type != 5)
        return fail(QStringLiteral("missing root directory entry"));

    // Mini stream lives in the root entry's regular-sector chain; mini-FAT
    // chains live in their own regular chain.
    const QByteArray miniStream = readChain(raw[0].start);
    std::vector<quint32> miniFat;
    {
        QByteArray mf;
        quint32 s = firstMiniFat;
        quint32 guard = 0;
        while (s != kEndOfChain && s != kFreeSect && guard++ <= numMiniFat + 1) {
            if (!sectorValid(s) || s >= fat.size())
                break;
            mf.append(bytes.constData() + sectorOffset(s), kSectorSize);
            s = fat[s];
        }
        for (int i = 0; i + 4 <= mf.size(); i += 4)
            miniFat.push_back(rdU32(mf, i));
    }
    auto readMiniChain = [&](quint32 start, quint64 size) -> QByteArray {
        QByteArray out;
        quint32 s = start;
        std::size_t guard = 0;
        while (s != kEndOfChain && s != kFreeSect) {
            if (s >= miniFat.size() || guard++ > miniFat.size())
                break;
            const int off = static_cast<int>(s) * kMiniSectorSize;
            if (off + kMiniSectorSize > miniStream.size())
                break;
            out.append(miniStream.constData() + off, kMiniSectorSize);
            s = miniFat[s];
        }
        return out.left(static_cast<int>(size));
    };

    // Rebuild the node tree from the directory BST.
    m_nodes.clear();
    m_root = 0;
    m_nodes.push_back(Node{ QStringLiteral("Root Entry"), true, {}, {} });

    // Map each raw directory id to a created node id, recursively.
    std::vector<int> created(raw.size(), -1);
    std::function<int(quint32)> makeNode = [&](quint32 rawId) -> int {
        if (rawId == kNoStream || rawId >= raw.size())
            return -1;
        if (created[rawId] != -1)
            return created[rawId];
        const RawEntry &e = raw[rawId];
        Node n;
        n.name = e.name;
        n.isStorage = (e.type == 1 || e.type == 5);
        if (e.type == 2) {  // stream
            n.data = (e.size < kMiniCutoff)
                         ? readMiniChain(e.start, e.size)
                         : readChain(e.start).left(static_cast<int>(e.size));
        }
        const int id = static_cast<int>(m_nodes.size());
        m_nodes.push_back(n);
        created[rawId] = id;
        return id;
    };

    // Collect a sibling BST (left/right) under a parent node.
    std::function<void(quint32, int)> addSubtree = [&](quint32 rawId, int parentNode) {
        if (rawId == kNoStream || rawId >= raw.size())
            return;
        const int nodeId = makeNode(rawId);
        if (nodeId < 0)
            return;
        m_nodes[parentNode].children.push_back(nodeId);
        const RawEntry &e = raw[rawId];
        addSubtree(e.left, parentNode);
        addSubtree(e.right, parentNode);
        if (m_nodes[nodeId].isStorage)
            addSubtree(e.child, nodeId);
    };
    addSubtree(raw[0].child, m_root);

    m_valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Tree access
// ---------------------------------------------------------------------------

int CompoundFile::findChild(int parent, const QString &name) const
{
    if (parent < 0 || parent >= static_cast<int>(m_nodes.size()))
        return -1;
    for (int c : m_nodes[parent].children)
        if (m_nodes[c].name == name)
            return c;
    return -1;
}

int CompoundFile::resolve(const QStringList &path) const
{
    int cur = m_root;
    for (const QString &part : path) {
        cur = findChild(cur, part);
        if (cur < 0)
            return -1;
    }
    return cur;
}

bool CompoundFile::hasStorage(const QStringList &path) const
{
    const int id = resolve(path);
    return id >= 0 && m_nodes[id].isStorage;
}

bool CompoundFile::hasStream(const QStringList &path) const
{
    const int id = resolve(path);
    return id >= 0 && !m_nodes[id].isStorage;
}

QStringList CompoundFile::childNames(const QStringList &storagePath) const
{
    QStringList names;
    const int id = resolve(storagePath);
    if (id >= 0)
        for (int c : m_nodes[id].children)
            names << m_nodes[c].name;
    return names;
}

QByteArray CompoundFile::readStream(const QStringList &path) const
{
    const int id = resolve(path);
    if (id < 0 || m_nodes[id].isStorage)
        return QByteArray();
    return m_nodes[id].data;
}

// ---------------------------------------------------------------------------
// Building / writing
// ---------------------------------------------------------------------------

int CompoundFile::ensureChild(int parent, const QString &name, bool storage)
{
    int existing = findChild(parent, name);
    if (existing >= 0)
        return existing;
    Node n;
    n.name = name;
    n.isStorage = storage;
    const int id = static_cast<int>(m_nodes.size());
    m_nodes.push_back(n);
    m_nodes[parent].children.push_back(id);
    return id;
}

void CompoundFile::addStream(const QStringList &path, const QByteArray &data)
{
    if (path.isEmpty())
        return;
    if (m_root < 0) {
        m_root = 0;
        m_nodes.push_back(Node{ QStringLiteral("Root Entry"), true, {}, {} });
        m_valid = true;
    }
    int cur = m_root;
    for (int i = 0; i < path.size() - 1; ++i)
        cur = ensureChild(cur, path.at(i), true);
    const int leaf = ensureChild(cur, path.last(), false);
    m_nodes[leaf].data = data;
    m_nodes[leaf].isStorage = false;
}

QByteArray CompoundFile::toByteArray() const
{
    if (m_root < 0)
        return QByteArray();

    // Directory entries, with sibling BSTs built per [MS-CFB] ordering.
    struct DirEntry {
        QString name;
        quint8 type = 0;               // 1 storage, 2 stream, 5 root
        quint32 left = kNoStream, right = kNoStream, child = kNoStream;
        quint32 start = kEndOfChain;
        quint64 size = 0;
        int srcNode = -1;
    };
    std::vector<DirEntry> dir;

    std::function<int(int)> build = [&](int nodeId) -> int {
        const Node &n = m_nodes[nodeId];
        const int idx = static_cast<int>(dir.size());
        DirEntry e;
        e.name = n.name;
        e.type = (nodeId == m_root) ? 5 : (n.isStorage ? 1 : 2);
        e.srcNode = nodeId;
        if (!n.isStorage)
            e.size = static_cast<quint64>(n.data.size());
        dir.push_back(e);

        if (n.isStorage && !n.children.empty()) {
            std::vector<int> kids = n.children;
            std::sort(kids.begin(), kids.end(), [&](int a, int b) {
                return cfbNameLess(m_nodes[a].name, m_nodes[b].name);
            });
            std::vector<int> dirIds;
            dirIds.reserve(kids.size());
            for (int k : kids)
                dirIds.push_back(build(k));   // assigns each child its dir index

            std::function<int(int, int)> bst = [&](int lo, int hi) -> int {
                if (lo > hi)
                    return static_cast<int>(kNoStream);
                const int mid = (lo + hi) / 2;
                const int self = dirIds[mid];
                const int l = bst(lo, mid - 1);
                const int r = bst(mid + 1, hi);
                dir[self].left  = (l < 0) ? kNoStream : static_cast<quint32>(l);
                dir[self].right = (r < 0) ? kNoStream : static_cast<quint32>(r);
                return self;
            };
            dir[idx].child = static_cast<quint32>(bst(0, static_cast<int>(dirIds.size()) - 1));
        }
        return idx;
    };
    build(m_root);

    // ---- regular-sector allocator -----------------------------------------
    QByteArray bodyBuf;
    std::vector<quint32> fat;
    int nextSector = 0;

    auto allocChain = [&](const QByteArray &data) -> quint32 {
        if (data.isEmpty())
            return kEndOfChain;
        const int n = (data.size() + kSectorSize - 1) / kSectorSize;
        const int start = nextSector;
        QByteArray padded = data;
        padded.append(QByteArray(n * kSectorSize - data.size(), '\0'));
        bodyBuf.append(padded);
        if (static_cast<int>(fat.size()) < start + n)
            fat.resize(start + n, kFreeSect);
        for (int i = 0; i < n; ++i)
            fat[start + i] = (i == n - 1) ? kEndOfChain : static_cast<quint32>(start + i + 1);
        nextSector += n;
        return static_cast<quint32>(start);
    };

    // Pass 1: mini stream + mini-FAT for small streams.
    QByteArray miniStream;
    std::vector<quint32> miniFat;
    for (DirEntry &e : dir) {
        if (e.type != 2)
            continue;
        const QByteArray &data = m_nodes[e.srcNode].data;
        if (data.isEmpty() || data.size() >= static_cast<int>(kMiniCutoff))
            continue;
        const int n = (data.size() + kMiniSectorSize - 1) / kMiniSectorSize;
        const quint32 startMini = static_cast<quint32>(miniStream.size() / kMiniSectorSize);
        QByteArray padded = data;
        padded.append(QByteArray(n * kMiniSectorSize - data.size(), '\0'));
        miniStream.append(padded);
        for (int i = 0; i < n; ++i)
            miniFat.push_back((i == n - 1) ? kEndOfChain
                                           : startMini + static_cast<quint32>(i) + 1);
        e.start = startMini;   // mini sector index
    }

    // Pass 2: lay out regular chains. Order: mini stream, big streams, mini-FAT,
    // then the directory (which must be serialised after all starts are known).
    const quint32 rootStart = allocChain(miniStream);
    for (DirEntry &e : dir) {
        if (e.type != 2)
            continue;
        const QByteArray &data = m_nodes[e.srcNode].data;
        if (data.size() >= static_cast<int>(kMiniCutoff))
            e.start = allocChain(data);
        else if (data.isEmpty())
            e.start = kEndOfChain;
        // small streams already assigned mini start in pass 1
    }

    QByteArray miniFatBytes;
    for (quint32 v : miniFat) {
        char tmp[4];
        qToLittleEndian<quint32>(v, reinterpret_cast<uchar *>(tmp));
        miniFatBytes.append(tmp, 4);
    }
    const quint32 miniFatStart = allocChain(miniFatBytes);
    const quint32 numMiniFatSectors =
        miniFatBytes.isEmpty() ? 0 : (miniFatBytes.size() + kSectorSize - 1) / kSectorSize;

    // Root entry holds the mini stream.
    dir[0].start = rootStart;
    dir[0].size  = static_cast<quint64>(miniStream.size());

    // Serialise the directory (padded to whole sectors, 4 entries per sector).
    const int entriesPerSector = kSectorSize / kDirEntrySize;
    int paddedEntries = ((static_cast<int>(dir.size()) + entriesPerSector - 1)
                         / entriesPerSector) * entriesPerSector;
    QByteArray dirBytes(paddedEntries * kDirEntrySize, '\0');
    for (int i = 0; i < static_cast<int>(dir.size()); ++i) {
        const DirEntry &e = dir[i];
        const int o = i * kDirEntrySize;
        const QString nm = e.name.left(31);
        for (int c = 0; c < nm.size(); ++c)
            wrU16(dirBytes, o + c * 2, nm.at(c).unicode());
        wrU16(dirBytes, o + 0x40, static_cast<quint16>((nm.size() + 1) * 2));
        dirBytes[o + 0x42] = static_cast<char>(e.type);
        dirBytes[o + 0x43] = 1;   // black
        wrU32(dirBytes, o + 0x44, e.left);
        wrU32(dirBytes, o + 0x48, e.right);
        wrU32(dirBytes, o + 0x4C, e.child);
        wrU32(dirBytes, o + 0x74, e.start);
        wrU64(dirBytes, o + 0x78, e.size);
    }
    // Unused directory slots: type 0 (unknown), all pointers NOSTREAM.
    for (int i = static_cast<int>(dir.size()); i < paddedEntries; ++i) {
        const int o = i * kDirEntrySize;
        wrU32(dirBytes, o + 0x44, kNoStream);
        wrU32(dirBytes, o + 0x48, kNoStream);
        wrU32(dirBytes, o + 0x4C, kNoStream);
    }
    const quint32 dirStart = allocChain(dirBytes);

    // ---- FAT sectors (fixpoint over their own count) -----------------------
    const int bodySectors = nextSector;
    const int entriesPerFat = kSectorSize / 4;
    int fatCount = 0;
    while (true) {
        const int total = bodySectors + fatCount;
        const int needed = (total + entriesPerFat - 1) / entriesPerFat;
        if (needed == fatCount)
            break;
        fatCount = needed;
    }
    if (fatCount > kHeaderDifatCount)
        return QByteArray();   // would need DIFAT sectors; out of scope for the scaffold

    fat.resize(fatCount * entriesPerFat, kFreeSect);
    for (int i = 0; i < fatCount; ++i)
        fat[bodySectors + i] = kFatSect;

    QByteArray fatBytes(fatCount * kSectorSize, '\0');
    for (int i = 0; i < static_cast<int>(fat.size()); ++i)
        wrU32(fatBytes, i * 4, fat[i]);

    // ---- header ------------------------------------------------------------
    QByteArray header(kSectorSize, '\0');
    std::memcpy(header.data(), kSignature, 8);
    wrU16(header, 0x18, 0x003E);   // minor version
    wrU16(header, 0x1A, 0x0003);   // major version (3)
    wrU16(header, 0x1C, 0xFFFE);   // byte order
    wrU16(header, 0x1E, 9);        // sector shift -> 512
    wrU16(header, 0x20, 6);        // mini sector shift -> 64
    wrU32(header, 0x2C, static_cast<quint32>(fatCount));
    wrU32(header, 0x30, dirStart);
    wrU32(header, 0x38, kMiniCutoff);
    wrU32(header, 0x3C, miniFatStart);
    wrU32(header, 0x40, numMiniFatSectors);
    wrU32(header, 0x44, kEndOfChain);   // first DIFAT sector (none)
    wrU32(header, 0x48, 0);             // number of DIFAT sectors
    for (int i = 0; i < kHeaderDifatCount; ++i)
        wrU32(header, 0x4C + i * 4,
              i < fatCount ? static_cast<quint32>(bodySectors + i) : kFreeSect);

    QByteArray out;
    out.reserve(header.size() + bodyBuf.size() + fatBytes.size());
    out.append(header);
    out.append(bodyBuf);
    out.append(fatBytes);
    return out;
}
