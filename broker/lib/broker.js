// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// The Schedule Vault sign-in broker: trades a Supabase Auth sign-in for a
// PostgreSQL password that works for five minutes (see the Vault's migration
// 073_sign_in_broker.sql, which decides which login, if any, it opens).
//
//   GET  /api/vault-credential  -> { supabaseUrl, anonKey }, for desktops
//   POST /api/vault-credential
//   Authorization: Bearer <Supabase access token>
//   -> 200 { login, user, password, validUntil, host?, port?, database? }
//      user is the name to connect with: login, or login.<project ref> when
//      desktops connect through Supabase's shared pooler, which needs it.
//      401 the token isn't a current Supabase sign-in
//      403 the sign-in doesn't open a Vault login
//      429 too many sign-ins for this person
//
// The password is made here and only its SCRAM verifier is sent to the
// database. The response is the only place it ever appears.

import { createHash, createHmac, pbkdf2Sync, randomBytes } from 'node:crypto';

// ── Credentials ──

export function newPassword() {
  return randomBytes(32).toString('base64url');
}

// PostgreSQL's SCRAM-SHA-256 verifier (RFC 5802, RFC 7677):
// SCRAM-SHA-256$<iterations>:<salt>$<StoredKey>:<ServerKey>
export function scramVerifier(password, salt = randomBytes(16), iterations = 4096) {
  const salted = pbkdf2Sync(Buffer.from(password.normalize('NFKC'), 'utf8'), salt, iterations, 32, 'sha256');
  const hmac = (key, text) => createHmac('sha256', key).update(text).digest();
  const storedKey = createHash('sha256').update(hmac(salted, 'Client Key')).digest();
  const serverKey = hmac(salted, 'Server Key');
  return `SCRAM-SHA-256$${iterations}:${salt.toString('base64')}$${storedKey.toString('base64')}:${serverKey.toString('base64')}`;
}

// The shared pooler (Supavisor) reads the project from the username:
// <role>.<project ref>. config.poolerRef, when set, is that ref.
export function connectionUser(login, poolerRef) {
  return poolerRef ? `${login}.${poolerRef}` : login;
}

// The ref to add: VAULT_POOLER_REF, or, when VAULT_HOST is Supabase's
// shared pooler, the project's subdomain in SUPABASE_URL.
export function poolerRefFor({ poolerRef, vaultHost, supabaseUrl }) {
  if (poolerRef) return poolerRef;
  if (!/\.pooler\.supabase\.com$/i.test(vaultHost ?? '')) return null;
  return /^https:\/\/([a-z0-9]+)\.supabase\.co\b/i.exec(supabaseUrl ?? '')?.[1] ?? null;
}

// ── Who signed in ──

// What identifies the organization behind one of the user's provider
// accounts: the Entra tenant, the Google Workspace domain, or the issuer.
export function directoryOf(identity) {
  const data = identity?.identity_data ?? {};
  const claims = data.custom_claims ?? {};
  switch (identity?.provider) {
    case 'azure': {
      const tenant = claims.tid ?? /^https:\/\/login\.microsoftonline\.com\/([^/]+)\//.exec(data.iss ?? '')?.[1];
      return tenant ?? null;
    }
    case 'google':
      return claims.hd ?? data.hd ?? null;  // personal Google accounts have none
    case 'email':
    case 'phone':
    case 'anonymous':
      return null;                          // not an organization's account
    default:
      return data.iss ?? null;
  }
}

// The provider accounts the broker may try, each with its directory and a
// verified email to match the Vault person by.
export function candidatesOf(user) {
  const out = [];
  for (const identity of user?.identities ?? []) {
    const directory = directoryOf(identity);
    if (!directory) continue;
    const data = identity.identity_data ?? {};
    const email = data.email && data.email_verified !== false ? data.email
                : user.email && user.email_confirmed_at ? user.email : null;
    out.push({ provider: identity.provider, directory, email });
  }
  return out;
}

// Asks Supabase Auth who the token belongs to; null when it isn't current.
export async function fetchSupabaseUser(supabaseUrl, anonKey, accessToken, fetchImpl = fetch) {
  const response = await fetchImpl(`${supabaseUrl}/auth/v1/user`, {
    headers: { apikey: anonKey, authorization: `Bearer ${accessToken}` },
  });
  if (response.status === 401 || response.status === 403) return null;
  if (!response.ok) throw new Error(`Supabase Auth answered ${response.status}`);
  return response.json();
}

// ── The request ──

export class BrokerError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

// config: { supabaseUrl, anonKey, poolerRef?, vault: { host, port, database } }
// deps:   { fetchUser(token) -> user|null, issue({issuer, subject, email, provider, directory, verifier}) -> {login, valid_until} }
export async function issueCredential(authorization, config, deps) {
  const token = /^Bearer\s+(\S+)$/i.exec(authorization ?? '')?.[1];
  if (!token) throw new BrokerError(401, 'Sign in first');
  const user = await deps.fetchUser(token);
  if (!user?.id) throw new BrokerError(401, 'Sign in again');

  const issuer = `${config.supabaseUrl.replace(/\/+$/, '')}/auth/v1`;
  const refused = 'This sign-in doesn\'t open a Vault login';
  for (const candidate of candidatesOf(user)) {
    const password = newPassword();
    try {
      const row = await deps.issue({ issuer, subject: user.id, ...candidate, verifier: scramVerifier(password) });
      return {
        login: row.login,
        user: connectionUser(row.login, config.poolerRef),
        password,
        validUntil: new Date(row.valid_until).toISOString(),
        ...config.vault,
      };
    } catch (error) {
      if (error.code === '28000') continue;                 // not this account; try the next
      if (error.code === '54000') throw new BrokerError(429, error.message);
      throw error;
    }
  }
  throw new BrokerError(403, refused);
}
