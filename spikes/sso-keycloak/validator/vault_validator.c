/*
 * vault_validator: PostgreSQL 18 OAuth validator for the Schedule Vault SSO spike.
 *
 * It asks the identity provider whether a bearer token is live (RFC 7662 token
 * introspection), then lets the connection in only when the token was issued by
 * the configured issuer, names the configured audience, and carries a
 * vault_login claim equal to the PostgreSQL role being opened.
 *
 * Settings (postgresql.conf, reload to change):
 *   vault_validator.issuer          expected "iss"
 *   vault_validator.introspect_url  introspection endpoint
 *   vault_validator.client_id       confidential client allowed to introspect
 *   vault_validator.client_secret
 *   vault_validator.audience        required "aud" entry
 *   vault_validator.login_claim     claim naming the role (default vault_login)
 */
#include "postgres.h"

#include <curl/curl.h>

#include "fmgr.h"
#include "libpq/oauth.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/jsonb.h"

PG_MODULE_MAGIC_EXT(.name = "vault_validator", .version = "0.1");

static char *cfg_issuer = NULL;
static char *cfg_introspect_url = NULL;
static char *cfg_client_id = NULL;
static char *cfg_client_secret = NULL;
static char *cfg_audience = NULL;
static char *cfg_login_claim = NULL;

static bool validate_token(const ValidatorModuleState *state, const char *token,
                           const char *role, ValidatorModuleResult *res);

static const OAuthValidatorCallbacks callbacks = {
    PG_OAUTH_VALIDATOR_MAGIC,
    .validate_cb = validate_token,
};

const OAuthValidatorCallbacks *
_PG_oauth_validator_module_init(void)
{
    return &callbacks;
}

void
_PG_init(void)
{
    DefineCustomStringVariable("vault_validator.issuer", "Expected token issuer.", NULL,
                               &cfg_issuer, "", PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("vault_validator.introspect_url", "Token introspection endpoint.", NULL,
                               &cfg_introspect_url, "", PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("vault_validator.client_id", "Introspection client id.", NULL,
                               &cfg_client_id, "", PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("vault_validator.client_secret", "Introspection client secret.", NULL,
                               &cfg_client_secret, "", PGC_SIGHUP, GUC_SUPERUSER_ONLY, NULL, NULL, NULL);
    DefineCustomStringVariable("vault_validator.audience", "Required audience.", NULL,
                               &cfg_audience, "vault-db", PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("vault_validator.login_claim", "Claim naming the database role.", NULL,
                               &cfg_login_claim, "vault_login", PGC_SIGHUP, 0, NULL, NULL, NULL);
    MarkGUCPrefixReserved("vault_validator");
}

static size_t
collect_body(char *data, size_t size, size_t nmemb, void *userp)
{
    StringInfo buf = (StringInfo) userp;
    size_t n = size * nmemb;

    if (buf->len + n > 1024 * 1024)
        return 0;               /* refuse oversized responses */
    appendBinaryStringInfo(buf, data, n);
    return n;
}

static bool
introspect(const char *token, StringInfo body)
{
    CURL *curl = curl_easy_init();
    CURLcode rc;
    long status = 0;
    char *escaped;
    StringInfoData form;

    if (!curl)
        return false;
    escaped = curl_easy_escape(curl, token, 0);
    initStringInfo(&form);
    appendStringInfo(&form, "token=%s&token_type_hint=access_token", escaped);
    curl_free(escaped);

    curl_easy_setopt(curl, CURLOPT_URL, cfg_introspect_url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form.data);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long) CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg_client_id);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg_client_secret);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    rc = curl_easy_perform(curl);
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    pfree(form.data);

    if (rc != CURLE_OK)
    {
        ereport(LOG, (errmsg("vault_validator: introspection failed: %s", curl_easy_strerror(rc))));
        return false;
    }
    if (status != 200)
    {
        ereport(LOG, (errmsg("vault_validator: introspection returned HTTP %ld", status)));
        return false;
    }
    return true;
}

static JsonbValue *
field(Jsonb *doc, const char *key)
{
    JsonbValue k;

    k.type = jbvString;
    k.val.string.val = (char *) key;
    k.val.string.len = strlen(key);
    return findJsonbValueFromContainer(&doc->root, JB_FOBJECT, &k);
}

static bool
string_equals(JsonbValue *v, const char *expected)
{
    return v && v->type == jbvString
        && v->val.string.len == (int) strlen(expected)
        && strncmp(v->val.string.val, expected, v->val.string.len) == 0;
}

static char *
string_copy(JsonbValue *v)
{
    return (v && v->type == jbvString) ? pnstrdup(v->val.string.val, v->val.string.len) : NULL;
}

static bool
audience_ok(Jsonb *doc)
{
    JsonbValue *aud = field(doc, "aud");
    JsonbIterator *it;
    JsonbValue v;
    JsonbIteratorToken tok;

    if (!aud)
        return false;
    if (aud->type == jbvString)
        return string_equals(aud, cfg_audience);
    if (aud->type != jbvBinary)
        return false;

    it = JsonbIteratorInit(aud->val.binary.data);
    while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
        if (tok == WJB_ELEM && string_equals(&v, cfg_audience))
            return true;
    return false;
}

static bool
validate_token(const ValidatorModuleState *state, const char *token,
               const char *role, ValidatorModuleResult *res)
{
    StringInfoData body;
    Jsonb *doc;
    JsonbValue *active;
    char *subject;

    res->authorized = false;
    res->authn_id = NULL;

    if (!*cfg_issuer || !*cfg_introspect_url || !*cfg_client_id)
    {
        ereport(LOG, (errmsg("vault_validator: issuer, introspect_url and client_id must be set")));
        return false;
    }

    initStringInfo(&body);
    if (!introspect(token, &body))
        return false;

    doc = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(body.data)));

    active = field(doc, "active");
    if (!active || active->type != jbvBool || !active->val.boolean)
    {
        ereport(LOG, (errmsg("vault_validator: token is not active")));
        return true;            /* well-formed answer: token is not live */
    }
    if (!string_equals(field(doc, "iss"), cfg_issuer))
    {
        ereport(LOG, (errmsg("vault_validator: token issuer does not match")));
        return true;
    }
    if (!audience_ok(doc))
    {
        ereport(LOG, (errmsg("vault_validator: token audience lacks \"%s\"", cfg_audience)));
        return true;
    }

    subject = string_copy(field(doc, "sub"));
    if (!subject)
        return true;

    /* Who the person is, recorded as the authenticated identity. */
    res->authn_id = psprintf("%s#%s", cfg_issuer, subject);

    /* Which login the token may open. */
    if (!string_equals(field(doc, cfg_login_claim), role))
    {
        ereport(LOG, (errmsg("vault_validator: token for %s may not open role \"%s\"",
                             res->authn_id, role)));
        return true;
    }

    res->authorized = true;
    return true;
}
