// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPP14MANIFEST_H
#define MPP14MANIFEST_H

#include <QStringList>

class CompoundFile;

// Microsoft Project duplicates rowset bookkeeping in each generation's parent
// Props stream.  The child stream headers can be internally valid while Project
// still hides the rows if these declarations are stale.
namespace Mpp14Manifest {

// Return every parent/child disagreement found in the 114 backend rowsets and
// the 214 view rowsets.  An empty list means the duplicated bookkeeping agrees.
QStringList audit(const CompoundFile &cf);

// Update the dynamic parent declarations from the rowsets currently in cf.
// Existing schema declarations and all opaque Props values remain untouched.
bool reconcile(CompoundFile &cf, QString *error = nullptr);

} // namespace Mpp14Manifest

#endif // MPP14MANIFEST_H
