// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Vercel function for the sign-in broker (see lib/broker.js).
//
// Environment:
//   SUPABASE_URL           https://<project>.supabase.co
//   SUPABASE_ANON_KEY      the project's public (anon) key
//   BROKER_DATABASE_URL    postgres://<broker login>:<password>@<host>:<port>/<database>
//                          (a login registered with vault.add_sign_in_broker)
//   BROKER_DATABASE_CA     optional: the database's CA certificate (PEM)
//   VAULT_HOST, VAULT_PORT, VAULT_DATABASE   optional: where desktops connect,
//                          returned with the credential

import pg from 'pg';
import { BrokerError, fetchSupabaseUser, issueCredential } from '../lib/broker.js';

let pool;
function database() {
  pool ??= new pg.Pool({
    connectionString: process.env.BROKER_DATABASE_URL,
    max: 2,
    idleTimeoutMillis: 10_000,
    // With BROKER_DATABASE_CA (Supabase's CA certificate, PEM) the server's
    // certificate is checked; without it the connection is still encrypted.
    ssl: process.env.BROKER_DATABASE_SSL === 'off' ? false
       : process.env.BROKER_DATABASE_CA ? { ca: process.env.BROKER_DATABASE_CA }
       : { rejectUnauthorized: false },
  });
  return pool;
}

const config = () => ({
  supabaseUrl: process.env.SUPABASE_URL,
  anonKey: process.env.SUPABASE_ANON_KEY,
  vault: Object.fromEntries(Object.entries({
    host: process.env.VAULT_HOST,
    port: process.env.VAULT_PORT ? Number(process.env.VAULT_PORT) : undefined,
    database: process.env.VAULT_DATABASE,
  }).filter(([, value]) => value !== undefined)),
});

export const deps = (settings) => ({
  fetchUser: (token) => fetchSupabaseUser(settings.supabaseUrl, settings.anonKey, token),
  issue: async ({ issuer, subject, email, provider, directory, verifier }) => {
    const { rows } = await database().query(
      'SELECT login, valid_until FROM vault.broker_issue_sign_in($1, $2, $3, $4, $5, $6)',
      [issuer, subject, email, provider, directory, verifier]);
    return rows[0];
  },
});

export default async function handler(request, response) {
  response.setHeader('cache-control', 'no-store');
  const settings = config();
  // Desktops learn which Supabase project to sign in to (both values are public).
  if (request.method === 'GET')
    return response.status(200).json({ supabaseUrl: settings.supabaseUrl, anonKey: settings.anonKey });
  if (request.method !== 'POST') {
    response.setHeader('allow', 'GET, POST');
    return response.status(405).json({ error: 'Use GET or POST' });
  }
  try {
    const credential = await issueCredential(request.headers.authorization, settings, deps(settings));
    return response.status(200).json(credential);
  } catch (error) {
    if (error instanceof BrokerError) return response.status(error.status).json({ error: error.message });
    console.error('vault-credential:', error.code ?? '', error.message);
    return response.status(500).json({ error: 'The sign-in service is unavailable. Try again later.' });
  }
}
