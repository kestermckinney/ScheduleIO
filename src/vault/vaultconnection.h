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
    // Single sign-on. signInIssuer is the sign-in service's issuer, exactly as
    // the server's pg_hba.conf names it; signInClientId is this app's client
    // there; signInProvider optionally names the organization's identity
    // provider so sign-in goes straight to it. An empty issuer means the login
    // signs in with a password.
    QString signInIssuer;
    QString signInClientId;
    QString signInProvider;

    static QString defaultSignInClientId() { return QStringLiteral("schedulevault"); }
    bool usesSingleSignOn() const { return !signInIssuer.trimmed().isEmpty(); }
    bool isComplete() const;
    // Host, port, database, and user: one saved password belongs to exactly this.
    QString identity() const;
    bool operator==(const VaultConnectionSettings &other) const;
    bool operator!=(const VaultConnectionSettings &other) const { return !(*this == other); }
};

SCHEDULEIO_EXPORT bool isLocalVaultHost(const QString &host);
// Verify certificates unless the server is this computer, which a local
// PostgreSQL install rarely has a certificate for.
SCHEDULEIO_EXPORT bool defaultVerifyTls(const QString &host);
// The libpq options every Vault connection uses.
SCHEDULEIO_EXPORT QString vaultConnectOptions(bool verifyTls);

// Reads postgresql://user[:password]@host[:port]/database[?sslmode=verify-full|prefer]
// with, for single sign-on, oauth_issuer=<issuer>, and optionally
// oauth_client_id=<client> (default schedulevault) and idp=<provider>.
// A URI without sslmode verifies certificates. On failure, error says what a
// usable URI looks like and settings and password are left untouched.
SCHEDULEIO_EXPORT bool parseVaultConnectionUri(const QString &input, VaultConnectionSettings &settings,
                                               QString &password, QString &error);

// Adds a QPSQL connection named connectionName configured for settings. The
// caller opens it with the password and removes it when done. With single
// sign-on, open it with no password after handing VaultSignIn a token.
SCHEDULEIO_EXPORT QSqlDatabase addVaultDatabase(const QString &connectionName,
                                                const VaultConnectionSettings &settings);

// Turns a libpq connection failure into the part a person can act on
// ("password authentication failed for user …").
SCHEDULEIO_EXPORT QString vaultConnectionFailure(const QSqlDatabase &database);

} // namespace schedule

#endif
