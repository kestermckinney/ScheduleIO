// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#ifndef SCHEDULE_VAULT_MIGRATIONS_H
#define SCHEDULE_VAULT_MIGRATIONS_H

#include "scheduleio_export.h"
#include <QString>
#include <QStringList>

class QSqlDatabase;

namespace schedule {

// Requires an open QPSQL connection as the Vault schema owner and no active
// transaction. A preview rolls back and returns the ordered pending filenames.
SCHEDULEIO_EXPORT bool migrateVault(const QSqlDatabase &database, bool preview,
                                    QStringList &pending, QString &error);
}

#endif
