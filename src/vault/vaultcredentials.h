// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#ifndef SCHEDULE_VAULT_CREDENTIALS_H
#define SCHEDULE_VAULT_CREDENTIALS_H

#include "scheduleio_export.h"

#include <QString>

// Vault passwords kept in the system credential store: Windows Credential
// Manager, the macOS keychain, or the Secret Service (secret-tool) elsewhere.
// application is the display name the entries are filed under, such as
// "Schedule Vault"; key names one password within it.
namespace schedule::vaultCredentials {
SCHEDULEIO_EXPORT bool store(const QString &application, const QString &key, const QString &password);
SCHEDULEIO_EXPORT bool read(const QString &application, const QString &key, QString &password);
SCHEDULEIO_EXPORT bool remove(const QString &application, const QString &key);
}

#endif
