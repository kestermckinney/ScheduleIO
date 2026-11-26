// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#ifndef SCHEDULE_VAULT_PROJECTMETADATA_H
#define SCHEDULE_VAULT_PROJECTMETADATA_H

#include <QByteArray>

namespace schedule {
class Project;

// The project metadata record excludes collections and view settings, which
// have separate storage. Decoding leaves those members untouched.
QByteArray encodeVaultProjectMetadata(const Project &project);
bool decodeVaultProjectMetadata(const QByteArray &bytes, Project &project);
}

#endif
