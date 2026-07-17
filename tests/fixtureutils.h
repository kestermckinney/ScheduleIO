// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#ifndef SCHEDULEIO_FIXTURE_DIR
#define SCHEDULEIO_FIXTURE_DIR ""
#endif

// Shared fixture discovery for every fixture-driven test. Recurses into
// subdirectories so sample sets like tests/fixtures/mpp_samples/01_.../ are
// picked up alongside the flat top-level fixtures.
namespace fixtures {

inline QStringList files(const QString &pattern)
{
    QStringList out;
    QDirIterator it(QStringLiteral(SCHEDULEIO_FIXTURE_DIR), { pattern },
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        out.append(it.next());
    out.sort();
    return out;
}

inline QStringList mppFiles() { return files(QStringLiteral("*.mpp")); }
inline QStringList xmlFiles() { return files(QStringLiteral("*.xml")); }

// Data-row label: path relative to the fixture root, so nested samples stay
// distinguishable ("mpp_samples/03_hierarchy_dependencies/... .mpp").
inline QString label(const QString &absPath)
{
    return QDir(QStringLiteral(SCHEDULEIO_FIXTURE_DIR)).relativeFilePath(absPath);
}

// The MS Project XML export paired with an .mpp: same directory, same base
// name. Empty if the pair is missing.
inline QString xmlSibling(const QString &mppPath)
{
    const QFileInfo fi(mppPath);
    const QString xml = fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".xml"));
    return QFile::exists(xml) ? xml : QString();
}

} // namespace fixtures
