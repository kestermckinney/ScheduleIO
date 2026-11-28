// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

import assert from 'node:assert/strict';
import { test } from 'node:test';
import pg from 'pg';
import { BrokerError, candidatesOf, directoryOf, issueCredential, scramVerifier } from '../lib/broker.js';

const config = { supabaseUrl: 'https://example.supabase.co/', anonKey: 'anon', vault: { host: 'db.example', port: 5432 } };
const azure = (tid, email = 'ann@acme.example') => ({
  provider: 'azure',
  identity_data: { iss: `https://login.microsoftonline.com/${tid}/v2.0`, email, email_verified: true, custom_claims: { tid } },
});

test('directories: Entra tenant, Workspace domain, issuer; none for email accounts', () => {
  assert.equal(directoryOf(azure('t-1')), 't-1');
  assert.equal(directoryOf({ provider: 'azure', identity_data: { iss: 'https://login.microsoftonline.com/t-2/v2.0' } }), 't-2');
  assert.equal(directoryOf({ provider: 'google', identity_data: { custom_claims: { hd: 'acme.example' } } }), 'acme.example');
  assert.equal(directoryOf({ provider: 'google', identity_data: { email: 'someone@gmail.com' } }), null);
  assert.equal(directoryOf({ provider: 'keycloak', identity_data: { iss: 'https://id.acme.example/realms/acme' } }),
               'https://id.acme.example/realms/acme');
  assert.equal(directoryOf({ provider: 'email', identity_data: { iss: 'x' } }), null);
});

test('unverified provider emails are not used to match a person', () => {
  const user = { email: 'ann@acme.example', email_confirmed_at: null,
                 identities: [{ provider: 'azure', identity_data: { email: 'ann@acme.example', email_verified: false,
                                                                    custom_claims: { tid: 't-1' } } }] };
  assert.deepEqual(candidatesOf(user), [{ provider: 'azure', directory: 't-1', email: null }]);
});

test('issues a credential for the first account the Vault accepts', async () => {
  const calls = [];
  const user = { id: 'user-1', identities: [{ provider: 'google', identity_data: { custom_claims: { hd: 'other.example' } } },
                                            azure('t-1')] };
  const credential = await issueCredential('Bearer token-1', config, {
    fetchUser: async (token) => (token === 'token-1' ? user : null),
    issue: async (request) => {
      calls.push(request);
      if (request.provider !== 'azure') throw Object.assign(new Error('refused'), { code: '28000' });
      return { login: 'ann', valid_until: '2026-11-27T12:05:00Z' };
    },
  });
  assert.equal(calls.length, 2);
  assert.deepEqual({ ...calls[1], verifier: undefined },
                   { issuer: 'https://example.supabase.co/auth/v1', subject: 'user-1', provider: 'azure',
                     directory: 't-1', email: 'ann@acme.example', verifier: undefined });
  assert.match(calls[1].verifier, /^SCRAM-SHA-256\$4096:/);
  assert.equal(credential.login, 'ann');
  assert.equal(credential.validUntil, '2026-11-27T12:05:00.000Z');
  assert.equal(credential.host, 'db.example');
  assert.ok(credential.password.length >= 40);
  assert.ok(!calls[1].verifier.includes(credential.password));
});

test('answers 401 without a current sign-in, 403 when refused, 429 when limited', async () => {
  const refuse = (code) => async () => { throw Object.assign(new Error('Too many sign-ins'), { code }); };
  const user = { id: 'u', identities: [azure('t-1')] };
  const status = (promise) => promise.then(() => 200, (error) => (error instanceof BrokerError ? error.status : error));
  assert.equal(await status(issueCredential(undefined, config, { fetchUser: async () => user })), 401);
  assert.equal(await status(issueCredential('Bearer x', config, { fetchUser: async () => null })), 401);
  assert.equal(await status(issueCredential('Bearer x', config, { fetchUser: async () => user, issue: refuse('28000') })), 403);
  assert.equal(await status(issueCredential('Bearer x', config, { fetchUser: async () => ({ id: 'u', identities: [] }) })), 403);
  assert.equal(await status(issueCredential('Bearer x', config, { fetchUser: async () => user, issue: refuse('54000') })), 429);
});

// Checks the verifier against a real server: set it on a temporary login and
// sign in with the password. Needs a role-creating login in PGHOST/PGUSER/PGPASSWORD.
test('PostgreSQL accepts the password for the verifier', { skip: !process.env.PGHOST && 'set PGHOST' }, async () => {
  const admin = new pg.Client({ database: 'postgres' });
  await admin.connect();
  const login = `broker_scram_${Date.now()}`;
  const password = 'correct horse battery staple ✓';
  try {
    await admin.query(`CREATE ROLE ${login} LOGIN PASSWORD '${scramVerifier(password)}' VALID UNTIL '${new Date(Date.now() + 60000).toISOString()}'`);
    const member = new pg.Client({ database: 'postgres', user: login, password });
    await member.connect();
    assert.equal((await member.query('SELECT current_user AS who')).rows[0].who, login);
    await member.end();
    const wrong = new pg.Client({ database: 'postgres', user: login, password: 'wrong' });
    await assert.rejects(wrong.connect(), /password authentication failed/);
  } finally {
    await admin.query(`DROP ROLE IF EXISTS ${login}`);
    await admin.end();
  }
});
