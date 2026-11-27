-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- Single sign-on through PostgreSQL 18 OAuth.
--
-- A Vault server may let members sign in with a token from the identity
-- service instead of a password (pg_hba "oauth" with a validator that allows
-- a token to open only the login assigned to its account). The server then
-- reports the person's identity as system_user = 'oauth:<issuer>#<subject>'.
--
-- bind_manager_context ties that identity to the Vault person:
--   * the first token sign-in records issuer and subject on vault.users
--     (people created by Manager start with issuer 'postgresql'). OpenID
--     Connect issuers are URLs, so a person whose issuer is an http(s) URL
--     is linked to single sign-on and every other person is not;
--   * later token sign-ins must carry the same identity;
--   * once an identity is recorded, password sign-in for that person stops.
-- Password logins without a recorded identity work exactly as before.

-- Only bind_manager_context, inside a token sign-in, may record an identity.
DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY link_sign_in_identity ON vault.users FOR UPDATE TO %I
         USING (id = vault.context_id(''app.current_user_id'')
                AND issuer !~ ''^https?://''
                AND current_setting(''app.link_sign_in_identity'', true) = ''on'')
         WITH CHECK (id = vault.context_id(''app.current_user_id''))', current_user);
END
$migration$;

CREATE OR REPLACE FUNCTION vault.bind_manager_context(tenant uuid)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        actor uuid;
        operator uuid;
        service uuid;
        tenant_status text;
        signed_in text := coalesce(system_user, '');
        token_identity text;
        token_issuer text;
        token_subject text;
        person_issuer text;
        person_subject text;
    BEGIN
        IF current_user IN (SELECT rolname FROM pg_roles WHERE rolsuper OR rolbypassrls) THEN
            RAISE EXCEPTION 'Desktop Vault operations require a migration owner subject to RLS'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        SELECT principal.user_id INTO actor
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login
          ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
        WHERE login.rolname = session_user AND login.rolcanlogin AND principal.active;

        IF actor IS NULL THEN
            RAISE EXCEPTION 'PostgreSQL login has no active Vault Manager identity'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        -- ── Single sign-on identity ──
        -- A token sign-in reaches here as system_user 'oauth:<issuer>#<subject>'
        -- (set by the server's OAuth validator, which only lets a token open the
        -- login its provider account was assigned). The first token sign-in
        -- records that identity on the person; every later one must match it,
        -- and a person with a recorded identity can no longer use a password.
        PERFORM set_config('app.current_user_id', actor::text, true);
        SELECT person.issuer, person.subject INTO person_issuer, person_subject
        FROM vault.users AS person WHERE person.id = actor;

        IF signed_in LIKE 'oauth:%' THEN
            token_identity := substr(signed_in, length('oauth:') + 1);
            IF person_issuer !~ '^https?://' THEN
                token_issuer := substring(token_identity FROM '^(.+)#[^#]+$');
                token_subject := substring(token_identity FROM '#([^#]+)$');
                IF token_issuer IS NULL OR token_subject IS NULL OR person_issuer = 'vault-service' THEN
                    RAISE EXCEPTION 'This sign-in can''t be linked to a Vault person'
                        USING ERRCODE = 'insufficient_privilege';
                END IF;
                PERFORM set_config('app.link_sign_in_identity', 'on', true);
                BEGIN
                    UPDATE vault.users SET issuer = token_issuer, subject = token_subject WHERE id = actor;
                EXCEPTION WHEN unique_violation THEN
                    RAISE EXCEPTION 'This single sign-on account is already linked to another Vault person'
                        USING ERRCODE = 'insufficient_privilege';
                END;
                PERFORM set_config('app.link_sign_in_identity', '', true);
            ELSIF person_issuer || '#' || person_subject IS DISTINCT FROM token_identity THEN
                RAISE EXCEPTION 'This sign-in belongs to a different person than this login'
                    USING ERRCODE = 'insufficient_privilege';
            END IF;
        ELSIF person_issuer ~ '^https?://' THEN
            RAISE EXCEPTION 'This person signs in with single sign-on'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        operator := vault.platform_operator_id();
        IF operator IS NOT NULL THEN
            IF tenant IS NOT NULL THEN
                SELECT account.user_id INTO service
                FROM vault.organization_service_accounts AS account WHERE account.organization_id = tenant;
                IF service IS NULL THEN
                    RAISE EXCEPTION 'Organization not found' USING ERRCODE = 'invalid_parameter_value';
                END IF;
                actor := service;
            END IF;
            PERFORM set_config('app.platform_actor_id', operator::text, true);
            PERFORM set_config('app.current_organization_id', coalesce(tenant::text, ''), true);
            PERFORM set_config('app.current_user_id', actor::text, true);
            RETURN;
        END IF;

        PERFORM set_config('app.platform_actor_id', '', true);
        PERFORM set_config('app.current_organization_id', coalesce(tenant::text, ''), true);
        PERFORM set_config('app.current_user_id', actor::text, true);
        IF tenant IS NOT NULL THEN
            SELECT organization.status INTO tenant_status
            FROM vault.organizations AS organization WHERE organization.id = tenant;
            IF tenant_status = 'suspended' THEN
                RAISE EXCEPTION 'This organization is suspended. Contact your service provider.'
                    USING ERRCODE = 'insufficient_privilege';
            END IF;
        END IF;
    END
    $$;

