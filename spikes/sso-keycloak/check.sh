#!/usr/bin/env bash
# Build the Qt probe and run the sign-in cases against ./up.sh.
set -uo pipefail
cd "$(dirname "$0")/probe"
cmake -S . -B build -G Ninja >/dev/null && cmake --build build >/dev/null || exit 1
export PGOAUTHDEBUG=UNSAFE   # the spike's Keycloak is plain HTTP
fail=0
expect() { # expect <CONNECTED|REJECTED> <label> args...
  local want=$1 label=$2; shift 2
  local out; out=$(QT_QPA_PLATFORM=offscreen timeout 60 ./build/sso_probe "$@" 2>/dev/null)
  if [[ $out == $want* ]]; then echo "ok   $label"; else echo "FAIL $label: $out"; fail=1; fi
}
expect CONNECTED "alice's token opens svt_alice"   svt_alice testuser1 svt_alice
expect REJECTED  "alice's token cannot open svt_bob" svt_alice testuser1 svt_bob
expect REJECTED  "made-up token is refused"         x x svt_alice not-a-real-token
expect CONNECTED "bob's token opens svt_bob"       svt_bob testuser1 svt_bob
../assign-login.sh svt_carol "" >/dev/null 2>&1 || true   # start from "not assigned"
export SSO_PROBE_BROWSER=$PWD/../scripted-browser.sh SSO_PASSWORD=testuser1
SSO_USER=svt_alice expect CONNECTED "browser sign-in (PKCE): alice opens svt_alice" --browser svt_alice
SSO_USER=svt_alice expect REJECTED  "browser sign-in (PKCE): alice cannot open svt_bob" --browser svt_bob
SSO_USER=svt_carol SSO_PROBE_IDP=acme expect REJECTED  "carol (organization provider) has no login until Vault assigns one" --browser svt_carol
../assign-login.sh svt_carol svt_carol >/dev/null
SSO_USER=svt_carol SSO_PROBE_IDP=acme expect CONNECTED "carol opens svt_carol once it is assigned" --browser svt_carol
SSO_USER=svt_alice SSO_PROBE_IDP=acme expect REJECTED  "an acme account named svt_alice cannot open alice's login" --browser svt_alice
exit $fail
