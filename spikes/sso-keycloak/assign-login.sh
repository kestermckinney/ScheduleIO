#!/usr/bin/env bash
# What Manager would do when it adds a person: write the Vault-assigned login
# onto their Keycloak user through the admin API. The person and their provider
# cannot change it (the attribute is admin-only in the realm's user profile).
#   assign-login.sh <keycloak username> <vault login>    (empty login removes it)
set -euo pipefail
kc=http://localhost:8080
admin=$(curl -sf -d grant_type=password -d client_id=admin-cli -d username=admin -d password=admin \
  $kc/realms/master/protocol/openid-connect/token | python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])')
user=$(curl -sf -H "Authorization: Bearer $admin" "$kc/admin/realms/vault/users?exact=true&username=$1")
body=$(python3 -c 'import json,sys
u=json.loads(sys.argv[1])[0]
a=u.setdefault("attributes",{})
if sys.argv[2]: a["vault_login"]=[sys.argv[2]]
else: a.pop("vault_login",None)
print(json.dumps(u))' "$user" "$2")
id=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])[0]["id"])' "$user")
curl -sf -X PUT -H "Authorization: Bearer $admin" -H 'Content-Type: application/json' -d "$body" "$kc/admin/realms/vault/users/$id"
echo "vault_login of $1 is now ${2:-unset}"
