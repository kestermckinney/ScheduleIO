// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#ifndef SCHEDULE_VAULT_CONNECTION_H
#define SCHEDULE_VAULT_CONNECTION_H

#include "scheduleio_export.h"

#include <QString>

class QSqlDatabase;

namespace schedule {

// Where a Vault lives and who signs in to it. The defaults are the ones a Vault
// created by Schedule Vault Manager on this computer uses, so a person setting
// up a connection usually only has to enter a login. Both Schedule Vault and
// the Manager start from these.
struct SCHEDULEIO_EXPORT VaultConnectionSettings
{
    static constexpr int DefaultPort = 5432;
    static QString defaultHost();
    static QString defaultDatabase();

    QString host = defaultHost();
    int port = DefaultPort;
    QString database = defaultDatabase();
    QString user;
    // Certificate and host-name checks, on by default for every server except
    // this computer's (see defaultVerifyTls()).
    bool verifyTls = false;
    // Single sign-on (see vaultsignin.h). signInBroker is the address of the
    // credential broker; signInProvider is the organization's identity
    // provider in Supabase Auth ("azure", "google", ...). With single sign-on,
    // user is the person's sign-in email, and the broker says which login it
    // opens. An empty broker means user is the login and has a password.
    QString signInBroker;
    QString signInProvider;

    bool usesSingleSignOn() const { return !signInBroker.trimmed().isEmpty(); }
    bool isComplete() const;
    // Host, port, database, and user: one saved password belongs to exactly this.
    QString identity() const;
    bool operator==(const VaultConnectionSettings &other) const;
    bool operator!=(const VaultConnectionSettings &other) const { return !(*this == other); }
};

SCHEDULEIO_EXPORT bool isLocalVaultHost(const QString &host);
// An https address, or http on this computer (for testing).
SCHEDULEIO_EXPORT bool isSignInAddress(const QString &address);
// Verify certificates unless the server is this computer, which a local
// PostgreSQL install rarely has a certificate for.
SCHEDULEIO_EXPORT bool defaultVerifyTls(const QString &host);
// The libpq options every Vault connection uses.
SCHEDULEIO_EXPORT QString vaultConnectOptions(bool verifyTls);

// Reads postgresql://user[:password]@host[:port]/database[?sslmode=verify-full|prefer]
// with, for single sign-on, broker=<https address> and provider=<name>.
// A URI without sslmode verifies certificates. On failure, error says what a
// usable URI looks like and settings and password are left untouched.
SCHEDULEIO_EXPORT bool parseVaultConnectionUri(const QString &input, VaultConnectionSettings &settings,
                                               QString &password, QString &error);

// Adds a QPSQL connection named connectionName configured for settings. The
// caller opens it with the password and removes it when done. With single
// sign-on, open it with the broker's login and password instead.
SCHEDULEIO_EXPORT QSqlDatabase addVaultDatabase(const QString &connectionName,
                                                const VaultConnectionSettings &settings);

// Turns a libpq connection failure into the part a person can act on
// ("password authentication failed for user …").
SCHEDULEIO_EXPORT QString vaultConnectionFailure(const QSqlDatabase &database);

// Signing in with an email (migration 075). An entry with "@" in a password
// connection is an email, not a login.
SCHEDULEIO_EXPORT bool looksLikeEmail(const QString &user);
// The Vault's lookup login, "<database>_lookup"; its password is its name.
SCHEDULEIO_EXPORT QString vaultLookupLogin(const QString &database);
// Connects as the lookup login and asks which login has this sign-in email.
// Blocks; returns false with error set when there is none or it can't ask.
SCHEDULEIO_EXPORT bool lookupVaultLogin(const VaultConnectionSettings &settings, const QString &email,
                                        QString &login, QString &error);

} // namespace schedule

#endif
