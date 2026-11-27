# SSO spike: Keycloak + PostgreSQL 18 OAuth

This spike proves that ScheduleVault can open its existing per-member PostgreSQL login with a Keycloak token instead of a password, through Qt's QPSQL driver.

## Run

```sh
./up.sh          # podman pod: Keycloak :8080, PostgreSQL 18 :5433 (localhost only)
./check.sh       # builds the probe and runs the sign-in cases
                 # (scripted-browser.sh fills in the sign-in pages with curl)
podman pod rm -f vault-sso-spike   # tear down
```

- **Keycloak admin console:** http://localhost:8080, signed in as admin / admin.
- **Realm `vault`:**
  - People: `svt_alice` and `svt_bob` (password `testuser1`).
  - Client `schedulevault`: public, with PKCE and the device grant.
  - Client `vault-db`: confidential, used for token introspection.
- **PostgreSQL:** the `postgres` superuser keeps its password, `testuser1`. Logins ending in `_platform` and members of `vault_password_sign_in` may use passwords; everyone else must use a token (`postgres/pg_hba.conf`).

## Pieces

| Part | What it does |
| --- | --- |
| `validator/vault_validator.c` | The OAuth validator loaded by the server. It sends the token to Keycloak's introspection endpoint and checks that it is active. It then checks `iss` and that `aud` includes `vault-db`. It allows the login only when the `vault_login` claim equals the role being opened. The identity it records is `issuer#subject`. |
| `postgres/pg_hba.conf` | `oauth ... delegate_ident_mapping=1`, so the validator, not a user-name map, decides which role a token may open. |
| `keycloak/vault-realm.json` | Mappers put `vault_login` (here the Keycloak username) and audience `vault-db` into access tokens. |
| `probe/` | A Qt console app. It gets a token, installs libpq's `PQsetAuthDataHook` with `dlsym`, and opens QPSQL with `oauth_issuer` and `oauth_client_id` in the connect options. |

## Results (2026-11-27)

| Case | Result |
| --- | --- |
| alice's token opens `svt_alice` | connected; `system_user = oauth:http://localhost:8080/realms/vault#<subject>` |
| alice's token tries to open `svt_bob` | rejected ("may not open role") |
| a made-up token | rejected (introspection: not active) |
| bob's token opens `svt_bob` | connected |
| browser sign-in (PKCE, loopback redirect): alice opens `svt_alice` | connected, refresh token issued |
| browser sign-in: alice tries to open `svt_bob` | rejected |
| carol signs in at realm `acme-idp` (an organization's own provider, federated into `vault`) before Vault assigns her a login | rejected (token has no `vault_login`) |
| the same, after `assign-login.sh svt_carol svt_carol` | connected as `svt_carol` |
| an `acme-idp` account named `svt_alice` tries to open alice's login | refused: Keycloak stops at "account already exists" and asks for alice's Vault password |

## Findings

- **QPSQL needs no changes.** Its connect options pass `oauth_issuer` and `oauth_client_id` straight through to libpq.
- **The hook covers the whole process.** Fedora's libpq 18.4 exports `PQsetAuthDataHook` even though it lacks the built-in browser and device flow (`libpq-oauth`). The app has to run its own sign-in anyway, so this suits us. The headers aren't needed; `dlsym` works.
- **A role check by the validator alone is not enough.** Without `delegate_ident_mapping=1`, PostgreSQL also requires the identity to equal the role name, and rejects the connection.
- **`system_user` carries `issuer#subject`.** That is exactly what `vault.users(issuer, subject)` stores. `bind_manager_context` could check it against the user mapped from `session_user`, which would add a second safeguard.
- **libpq refuses plain-HTTP issuers.** Set `PGOAUTHDEBUG=UNSAFE` only for this local spike; real deployments need HTTPS in front of Keycloak.
- **libpq retries the connection once** to fetch a token, which costs one extra round trip per connection.

- **The browser sign-in needs only QtCore, QtNetwork and QtGui (about 80 lines in `probe/main.cpp`).** It uses PKCE with a callback to a local port, so the app keeps no client secret. Qt NetworkAuth isn't installed here; it could replace this code later.
- **Each organization's provider is one Keycloak "identity provider" entry** (`identityProviders` in `vault-realm.json`). The app sends `kc_idp_hint=<alias>` so people go straight to their provider. Manager could create these entries through Keycloak's admin API.
- **Vault, not the organization's provider, decides which login a person may open.** `vault_login` is a Keycloak user attribute that only administrators may view or edit (set in the realm's user profile). `assign-login.sh` sets it the way Manager would. The provider only proves who the person is, so a new person from a provider gets no access until Vault assigns a login.
- **Same name, different person: the account isn't linked automatically.** Keycloak's first-broker-login flow doesn't link an incoming account to an existing user with the same name. In the product it's better to give each organization's people their own username prefix (for example `acme.`), so an identical name from two organizations can't block anyone.

## Next steps

1. Done (2026-11-27): migration 071 links and checks the `system_user` identity in `bind_manager_context`. ScheduleVaultManager's `tests/tst_sso_vault.cpp` runs it against this spike with a real Vault (`SCHEDULEIO_SSO_SPIKE_DIR=$PWD ctest -L sso` from the Manager build).
2. Done (2026-11-27): Manager's **People → Sign-in** tab (migration 072) switches an organization to single sign-on and assigns `vault_login` to provider accounts by sign-in email. Its client is `vault-manager` (secret `vault-manager-spike-secret`).
3. In ScheduleVault: use the browser sign-in from `probe/main.cpp`.
4. Try a SAML provider entry, plus SCIM or Keycloak events for removing people.
