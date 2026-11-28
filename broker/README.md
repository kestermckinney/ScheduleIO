# Schedule Vault sign-in broker

A Vercel function that turns a Supabase Auth sign-in into a PostgreSQL password that works for five minutes. People in single sign-on organizations use it to open their Vault login.

The database decides which login, if any, a sign-in opens. See `vault.broker_issue_sign_in` in `vault/migrations/073_sign_in_broker.sql` and "Single sign-on" in `docs/Design/VaultDataModel.md`. Organizations that use passwords don't involve the broker.

## Set up

### 1. Supabase Auth
1. **Turn on each organization's provider** under Authentication → Sign In / Providers. All of these work on the free plan:
   - **Microsoft (`azure`):** register one Entra app that accepts any organization's accounts (multitenant), and leave the Azure Tenant URL empty. The broker matches each person by their tenant ID, so one app covers every organization.
   - **Google (`google`):** the broker matches Google Workspace accounts by domain. Personal Gmail accounts have no domain and never match.
   - **Other OpenID Connect providers** Supabase supports, such as `keycloak` or `workos`: the broker matches on the provider's issuer.
2. **Allow the desktop's sign-in callback** under Authentication → URL Configuration → Redirect URLs: `http://127.0.0.1:*/callback/**`.

### 2. A broker login in the Vault database
Run as the Vault owner (on Supabase, `postgres`):

```sql
CREATE ROLE vault_broker LOGIN PASSWORD '<a long random password>';
SELECT vault.add_sign_in_broker('vault_broker');
```

The login can run `vault.broker_issue_sign_in` and nothing else. To revoke it, use `SELECT vault.remove_sign_in_broker('vault_broker');`.

### 3. Deploy this folder to Vercel
Set these environment variables:

| Variable | Value |
|---|---|
| `SUPABASE_URL` | `https://<project>.supabase.co` |
| `SUPABASE_ANON_KEY` | the project's public (anon) key |
| `BROKER_DATABASE_URL` | `postgres://vault_broker:<password>@<pooler host>:5432/postgres`. Use the session pooler or the direct connection, not the transaction pooler. |
| `BROKER_DATABASE_CA` | optional: Supabase's CA certificate (PEM), so the database's certificate is checked |
| `VAULT_HOST`, `VAULT_PORT`, `VAULT_DATABASE` | optional: where desktops connect, returned with each credential |
| `VAULT_POOLER_REF` | optional: the project ref for Supabase's shared pooler, which needs usernames of the form `<login>.<project ref>`. When `VAULT_HOST` is `*.pooler.supabase.com`, it comes from `SUPABASE_URL`. |

The endpoint is `https://<deployment>/api/vault-credential`.

### 4. Each single sign-on organization, in Manager
1. On the **Sign-in** page, choose single sign-on, the provider, and the directory. The directory is the Entra tenant ID, the Workspace domain, or the issuer.
2. Set each person's sign-in email.
3. Desktops connect with:
   - **Broker:** the endpoint address above;
   - **Provider:** the provider's name;
   - **User:** the person's email.

## API
- `GET /api/vault-credential` returns `{ supabaseUrl, anonKey }`, so desktops only need the broker's address.
- `POST /api/vault-credential` with `Authorization: Bearer <Supabase access token>` returns one of:
  - `200 { login, user, password, validUntil, host?, port?, database? }`, where `user` is the name to connect with (`login`, or `login.<project ref>` for the shared pooler);
  - `401` when the token isn't a current sign-in;
  - `403` when the sign-in doesn't open a login;
  - `429` when the person has made too many requests.

## Test
`npm test` runs the unit tests. With `PGHOST`, `PGUSER` and `PGPASSWORD` set to a login that can create roles, it also checks that PostgreSQL accepts the passwords the broker makes.
