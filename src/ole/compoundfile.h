// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMPOUNDFILE_H
#define COMPOUNDFILE_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <vector>

// Minimal reader/writer for the OLE2 / Compound File Binary container
// ([MS-CFB]) that .mpp files use (WINPROJ: StgOpenStorage / StgCreateDocfile,
// referenceapp.c:1359650, 1366534). Supports version-3 (512-byte sector)
// docfiles with FAT, mini-FAT and a directory BST -- enough to round-trip the
// storage tree of a project file.
//
// Reading is bounds-checked throughout (cycle-guarded chain walks) so that
// malformed input fails cleanly rather than crashing -- see plan, Layer 4.
class CompoundFile
{
public:
    CompoundFile() = default;

    // Cheap signature check, mirrors StgIsStorageFile.
    static bool looksLikeCompoundFile(const QByteArray &bytes);

    // Parse an in-memory docfile. Returns false (with errorString set) on any
    // structural problem.
    bool openFromData(const QByteArray &bytes);

    bool isValid() const { return m_valid; }
    QString errorString() const { return m_error; }

    // Tree navigation. Paths are storage/stream names from the root, e.g.
    // {"Project", "Task", "FixedData"}.
    bool hasStorage(const QStringList &path) const;
    bool hasStream(const QStringList &path) const;
    QStringList childNames(const QStringList &storagePath = {}) const;
    QByteArray readStream(const QStringList &path) const;

    // Build a tree for writing. Intermediate storages are created as needed.
    void addStream(const QStringList &path, const QByteArray &data);

    // Serialise the current tree to a valid version-3 docfile.
    QByteArray toByteArray() const;

private:
    struct Node {
        QString name;
        bool isStorage = false;
        QByteArray data;                  // streams only
        std::vector<int> children;        // indices into m_nodes (storages only)
    };

    int m_root = -1;                      // index of the synthetic "Root Entry"
    std::vector<Node> m_nodes;
    bool m_valid = false;
    QString m_error;

    int ensureChild(int parent, const QString &name, bool storage);
    int findChild(int parent, const QString &name) const;
    int resolve(const QStringList &path) const;   // -1 if not found
};

#endif // COMPOUNDFILE_H
