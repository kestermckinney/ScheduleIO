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

// Single sign-on for Vault connections, through a credential broker.
//
// A hosted Vault (Supabase) can't accept tokens itself, so people of a single
// sign-on organization:
//   1. sign in to Supabase Auth with their organization's identity provider:
//      signIn() opens the system browser (authorization code with PKCE,
//      RFC 7636) and Supabase redirects back to a one-time address on
//      127.0.0.1 (RFC 8252);
//   2. trade that sign-in for a PostgreSQL password that works for five
//      minutes: fetchCredential() asks the broker (settings.signInBroker),
//      which returns the login it opens;
//   3. open the connection with that login and password. A session stays
//      open after the password expires; a new connection needs a new one.
// refresh() renews the Supabase sign-in without the browser.
//
// The broker also says which Supabase project it trusts, so settings name
// only the broker and the organization's provider. Every call blocks with a
// nested event loop; call them from the thread that opens the connection.
struct SCHEDULEIO_EXPORT VaultSignInTokens
{
    QString accessToken;
    QString refreshToken;
    QDateTime expiresAt;  // UTC; invalid when the service didn't say

    // Usable for a request starting now, with a minute to spare.
    bool accessTokenFresh() const;
};

struct SCHEDULEIO_EXPORT VaultCredential
{
    QString login;
    QString password;
    QDateTime validUntil;  // UTC
};

namespace vaultSignIn {

// Opens the browser at the sign-in page; the default uses the system's.
using BrowserLauncher = std::function<bool(const QUrl &)>;
SCHEDULEIO_EXPORT void setBrowserLauncher(BrowserLauncher launcher);

// Signs in through the browser, waiting up to timeoutMs for the person.
SCHEDULEIO_EXPORT bool signIn(const VaultConnectionSettings &settings, VaultSignInTokens &tokens, QString &error,
                              int timeoutMs = 300000);
SCHEDULEIO_EXPORT bool refresh(const VaultConnectionSettings &settings, const QString &refreshToken,
                               VaultSignInTokens &tokens, QString &error);
// Trades a current sign-in for a short-lived password.
SCHEDULEIO_EXPORT bool fetchCredential(const VaultConnectionSettings &settings, const QString &accessToken,
                                       VaultCredential &credential, QString &error);

} // namespace vaultSignIn
} // namespace schedule

#endif
