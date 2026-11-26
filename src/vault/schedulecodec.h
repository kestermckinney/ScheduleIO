// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#ifndef SCHEDULE_VAULT_SCHEDULECODEC_H
#define SCHEDULE_VAULT_SCHEDULECODEC_H

#include "model/project.h"

namespace schedule {
QByteArray encodeVaultTask(const Task &record);
bool decodeVaultTask(const QByteArray &bytes, Task &record);

QByteArray encodeVaultResource(const Resource &record);
bool decodeVaultResource(const QByteArray &bytes, Resource &record);

QByteArray encodeVaultAssignment(const Assignment &record);
bool decodeVaultAssignment(const QByteArray &bytes, Assignment &record);

QByteArray encodeVaultProjectCollections(const Project &record);
bool decodeVaultProjectCollections(const QByteArray &bytes, Project &record);

} // namespace schedule

#endif
