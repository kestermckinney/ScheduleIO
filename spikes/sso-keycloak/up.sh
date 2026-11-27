#!/usr/bin/env bash
# Start Keycloak and an OAuth-enabled PostgreSQL 18 in one podman pod.
# Both see each other (and the host sees both) on localhost, so the token
# issuer URL is the same everywhere: http://localhost:8080/realms/vault
set -euo pipefail
cd "$(dirname "$0")"

POD=vault-sso-spike
podman pod rm -f "$POD" >/dev/null 2>&1 || true
podman build -q -t vault-sso-postgres -f postgres/Containerfile . >/dev/null

podman pod create --name "$POD" -p 127.0.0.1:8080:8080 -p 127.0.0.1:5433:5433 >/dev/null

podman run -d --pod "$POD" --name "$POD-keycloak" \
  -e KC_BOOTSTRAP_ADMIN_USERNAME=admin -e KC_BOOTSTRAP_ADMIN_PASSWORD=admin \
  -v "$PWD/keycloak:/opt/keycloak/data/import:Z,ro" \
  quay.io/keycloak/keycloak:26.4 start-dev --import-realm >/dev/null

podman run -d --pod "$POD" --name "$POD-postgres" \
  -e POSTGRES_PASSWORD=testuser1 \
  vault-sso-postgres -c config_file=/etc/postgresql/postgresql.oauth.conf >/dev/null

printf 'Waiting for Keycloak'
until curl -sf http://localhost:8080/realms/vault/.well-known/openid-configuration >/dev/null; do
  printf .; sleep 2
done
echo ' ready'
printf 'Waiting for PostgreSQL'
until podman exec "$POD-postgres" pg_isready -q -p 5433; do printf .; sleep 1; done
echo ' ready'
