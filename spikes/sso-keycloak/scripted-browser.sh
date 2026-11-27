#!/usr/bin/env bash
# Stand-in for a person at a browser: open the authorization URL, fill in the
# sign-in form with $SSO_USER / $SSO_PASSWORD, and follow the redirects back to
# the app's loopback address. If Keycloak stops on any other page (for example
# "account already exists"), tell the app the sign-in was abandoned.
set -uo pipefail
jar=$(mktemp); trap 'rm -f "$jar"' EXIT
page=$(curl -s -L -c "$jar" -b "$jar" "$1")   # -L: may hop to the organization's provider
action=$(grep -o 'action="[^"]*"' <<<"$page" | head -1 | sed 's/^action="//; s/"$//; s/&amp;/\&/g')
final=$(curl -s -L -c "$jar" -b "$jar" -o /dev/null -w '%{url_effective}' \
  --data-urlencode "username=$SSO_USER" --data-urlencode "password=$SSO_PASSWORD" \
  --data-urlencode "credentialId=" "$action")
if [[ $final != http://127.0.0.1:* ]]; then
  redirect=$(python3 -c 'import sys,urllib.parse as u; q=u.parse_qs(u.urlparse(sys.argv[1]).query); print(q["redirect_uri"][0]+"?error=access_denied&state="+q["state"][0])' "$1")
  curl -s -o /dev/null "$redirect"
fi
