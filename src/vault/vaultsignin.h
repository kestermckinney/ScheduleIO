// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#ifndef SCHEDULE_VAULT_SIGNIN_H
#define SCHEDULE_VAULT_SIGNIN_H

#include "scheduleio_export.h"
#include "vaultconnection.h"

#include <QDateTime>
#include <QString>
#include <QUrl>
#include <functional>

namespace schedule {

// Single sign-on for Vault connections (PostgreSQL 18 OAuth).
//
// A Vault server that accepts tokens asks libpq for one while connecting.
// ScheduleIO answers through libpq's process-wide PQsetAuthDataHook with the
// token handed to setAccessToken() for that issuer, so the QPSQL connections
// addVaultDatabase() creates need nothing else. The hook is found in the libpq
// already loaded by the QPSQL driver; libpq 17 and earlier don't have it.
//
// signIn() runs the desktop sign-in: the system browser opens the sign-in
// service (authorization code with PKCE, RFC 7636), which redirects back to a
// one-time address on 127.0.0.1 (RFC 8252). refresh() trades a refresh token
// for a new access token without the browser. Both block, with a nested event
// loop, so call them from the thread that will open the connection.
struct SCHEDULEIO_EXPORT VaultSignInTokens
{
    QString accessToken;
    QString refreshToken;
    QDateTime expiresAt;  // UTC; invalid when the service didn't say

    // Usable for a connection starting now, with a minute to spare.
    bool accessTokenFresh() const;
};

namespace vaultSignIn {

// Installs the token hook once. False with error when libpq lacks it.
SCHEDULEIO_EXPORT bool installTokenHook(QString &error);
// The token the hook hands out for connections to issuer; empty forgets it.
SCHEDULEIO_EXPORT void setAccessToken(const QString &issuer, const QString &token);

// Opens the browser at the sign-in page; the default uses openBrowser().
using BrowserLauncher = std::function<bool(const QUrl &)>;
SCHEDULEIO_EXPORT void setBrowserLauncher(BrowserLauncher launcher);

// Signs in through the browser, waiting up to timeoutMs for the person.
SCHEDULEIO_EXPORT bool signIn(const VaultConnectionSettings &settings, VaultSignInTokens &tokens, QString &error,
                              int timeoutMs = 300000);
SCHEDULEIO_EXPORT bool refresh(const VaultConnectionSettings &settings, const QString &refreshToken,
                               VaultSignInTokens &tokens, QString &error);

} // namespace vaultSignIn
} // namespace schedule

#endif
