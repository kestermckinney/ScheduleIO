-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- How each organization's people sign in: with passwords (the default), or
-- with single sign-on through the organization's own identity provider.
--
-- Password sign-in is granted by membership in the cluster role
-- vault_password_sign_in, which pg_hba.conf on an SSO-enabled server lists
-- ahead of its "oauth" line. Servers without single sign-on never mention it,
-- so the membership is harmless there.
--
-- Switching an organization to single sign-on removes its members' passwords
-- and their password sign-in. Their logins stay; a token assigned to a login
-- (see migration 071) is the only way to open it. People added afterwards get
-- logins without a password, and a sign-in email that Manager matches with
-- the accounts arriving from the organization's identity provider.

-- ── Settings ──
CREATE TABLE vault.organization_sign_in (
    organization_id uuid PRIMARY KEY REFERENCES vault.organizations ON DELETE CASCADE,
    method text NOT NULL DEFAULT 'password' CHECK (method IN ('password', 'sso')),
    -- The identity provider's alias in the sign-in service (for Keycloak, the
    -- identity provider alias, which ScheduleVault sends as kc_idp_hint).
    provider text CHECK (provider IS NULL OR provider ~ '^[a-z0-9][a-z0-9_.-]{0,63}$'),
    updated_at timestamptz NOT NULL DEFAULT now(),
    CHECK (method = 'password' OR provider IS NOT NULL)
);
ALTER TABLE vault.organization_sign_in ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.organization_sign_in FORCE ROW LEVEL SECURITY;

-- The address a person's identity-provider account carries, used only to
-- match that account with their Vault login.
ALTER TABLE vault.users ADD COLUMN sign_in_email text
    CHECK (sign_in_email IS NULL OR sign_in_email ~ '^[^@[:space:]]+@[^@[:space:]]+$');

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY manage_organization_sign_in ON vault.organization_sign_in FOR ALL TO %I
         USING (organization_id = vault.context_id(''app.current_organization_id'')
                AND vault.is_organization_admin(organization_id))
         WITH CHECK (organization_id = vault.context_id(''app.current_organization_id'')
                     AND vault.is_organization_admin(organization_id))', current_user);
    -- Organization admins see and set their own people's sign-in email.
    EXECUTE format(
        'CREATE POLICY admin_read_sign_in_people ON vault.users FOR SELECT TO %I
         USING (home_organization_id = vault.context_id(''app.current_organization_id'')
                AND vault.is_organization_admin(home_organization_id))', current_user);
    EXECUTE format(
        'CREATE POLICY admin_set_sign_in_email ON vault.users FOR UPDATE TO %I
         USING (home_organization_id = vault.context_id(''app.current_organization_id'')
                AND vault.is_organization_admin(home_organization_id))
         WITH CHECK (home_organization_id = vault.context_id(''app.current_organization_id''))', current_user);
END
$migration$;

CREATE FUNCTION vault.organization_uses_sso(tenant uuid)
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT coalesce((SELECT setting.method = 'sso' FROM vault.organization_sign_in AS setting
                         WHERE setting.organization_id = tenant), false)
    $$;
REVOKE ALL ON FUNCTION vault.organization_uses_sso(uuid) FROM PUBLIC;

-- ── Password sign-in membership ──
-- Grants or revokes vault_password_sign_in for a login. The role is created
-- on first use; on a server where someone else created it, the Vault owner
-- needs ADMIN OPTION on it.
CREATE FUNCTION vault.sync_password_sign_in(desktop_login name, allowed boolean)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'vault_password_sign_in') THEN
            IF NOT allowed THEN
                RETURN;
            END IF;
            EXECUTE 'CREATE ROLE vault_password_sign_in NOLOGIN';
        END IF;
        IF NOT pg_has_role(current_user, 'vault_password_sign_in', 'USAGE WITH ADMIN OPTION')
           AND NOT pg_has_role(current_user, 'vault_password_sign_in', 'MEMBER WITH ADMIN OPTION') THEN
            RAISE EXCEPTION 'The Vault owner can''t manage password sign-in on this server'
                USING ERRCODE = 'insufficient_privilege',
                      HINT = format('A server administrator can run: GRANT vault_password_sign_in TO %I WITH ADMIN OPTION',
                                    current_user);
        END IF;
        IF allowed THEN
            EXECUTE format('GRANT vault_password_sign_in TO %I', desktop_login);
        ELSE
            EXECUTE format('REVOKE vault_password_sign_in FROM %I', desktop_login);
        END IF;
    END
    $$;
REVOKE ALL ON FUNCTION vault.sync_password_sign_in(name, boolean) FROM PUBLIC;

-- Logins may now be created without a password (single sign-on only).
CREATE OR REPLACE FUNCTION vault.create_vault_login(desktop_login name, password_verifier text)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        IF nullif(btrim(desktop_login), '') IS NULL OR desktop_login ~ '^pg_' THEN
            RAISE EXCEPTION 'Enter a valid login name' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF password_verifier IS NOT NULL
           AND password_verifier !~ '^SCRAM-SHA-256\$[0-9]+:[A-Za-z0-9+/=]+\$[A-Za-z0-9+/=]+:[A-Za-z0-9+/=]+$' THEN
            RAISE EXCEPTION 'A SCRAM-SHA-256 password verifier is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = current_user AND rolcreaterole) THEN
            RAISE EXCEPTION 'The Vault schema owner needs CREATEROLE to create logins'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF EXISTS (SELECT FROM pg_roles WHERE rolname = desktop_login) THEN
            RAISE EXCEPTION 'The login "%" already exists. Choose another name.', desktop_login
                USING ERRCODE = 'duplicate_object';
        END IF;
        IF password_verifier IS NULL THEN
            EXECUTE format('CREATE ROLE %I LOGIN NOSUPERUSER NOBYPASSRLS NOCREATEDB '
                           'NOCREATEROLE NOREPLICATION', desktop_login);
        ELSE
            EXECUTE format('CREATE ROLE %I LOGIN PASSWORD %L NOSUPERUSER NOBYPASSRLS NOCREATEDB '
                           'NOCREATEROLE NOREPLICATION', desktop_login, password_verifier);
        END IF;
    END
    $$;

-- Platform administrators always keep a password (the way in when the
-- sign-in service is down).
CREATE OR REPLACE FUNCTION vault.platform_create_login(desktop_login name, password_verifier text)
    RETURNS void
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        operator uuid := vault.require_platform_operator();
    BEGIN
        IF password_verifier IS NULL THEN
            RAISE EXCEPTION 'A SCRAM-SHA-256 password verifier is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM vault.create_vault_login(desktop_login, password_verifier);
        PERFORM vault.sync_password_sign_in(desktop_login, true);
        INSERT INTO vault_admin.platform_events (operator_id, operator_login, operation, details)
        VALUES (operator, session_user, 'platform.login_created', jsonb_build_object('login', desktop_login));
    END
    $$;

-- Members of a password organization get a password; members of a single
-- sign-on organization must not.
CREATE OR REPLACE FUNCTION vault.manager_create_login(tenant uuid, desktop_login name, password_verifier text)
    RETURNS void
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        sso boolean;
    BEGIN
        IF NOT has_function_privilege(session_user,
                'vault.manager_enroll_member(uuid,text,name,boolean,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        sso := vault.organization_uses_sso(tenant);
        IF sso AND password_verifier IS NOT NULL THEN
            RAISE EXCEPTION 'This organization signs in with single sign-on, so its people don''t get passwords'
                USING ERRCODE = 'invalid_parameter_value';
        ELSIF NOT sso AND password_verifier IS NULL THEN
            RAISE EXCEPTION 'A SCRAM-SHA-256 password verifier is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM vault.create_vault_login(desktop_login, password_verifier);
        PERFORM vault.sync_password_sign_in(desktop_login, NOT sso);
        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, vault.context_id('app.current_user_id'), 'organization.login_created',
                jsonb_build_object('login', desktop_login, 'single_sign_on', sso));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

-- ── Reading and changing the setting ──
CREATE FUNCTION vault.manager_sign_in(tenant uuid)
    RETURNS jsonb
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        result jsonb;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        SELECT jsonb_build_object(
                   'method', coalesce(setting.method, 'password'),
                   'provider', setting.provider,
                   'people', coalesce((
                       SELECT jsonb_agg(jsonb_build_object(
                                  'id', person.id,
                                  'name', coalesce(member.display_name, person.display_name),
                                  'login', principal.database_role_name,
                                  'email', person.sign_in_email,
                                  'linked', person.issuer ~ '^https?://')
                              ORDER BY coalesce(member.display_name, person.display_name))
                       FROM vault.organization_members AS member
                       JOIN vault.users AS person ON person.id = member.user_id
                       JOIN vault.manager_principals AS principal
                         ON principal.user_id = person.id AND principal.active
                       WHERE member.organization_id = tenant AND member.active
                         AND person.home_organization_id = tenant
                         AND NOT vault.is_service_account(person.id)), '[]'::jsonb))
        INTO result
        FROM (SELECT tenant AS organization_id) AS wanted
        LEFT JOIN vault.organization_sign_in AS setting USING (organization_id);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN result;
    END
    $$;
REVOKE ALL ON FUNCTION vault.manager_sign_in(uuid) FROM PUBLIC;

CREATE FUNCTION vault.manager_set_sign_in(tenant uuid, new_method text, new_provider text)
    RETURNS void
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        member_login name;
    BEGIN
        IF NOT has_function_privilege(session_user,
                'vault.manager_enroll_member(uuid,text,name,boolean,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        IF NOT EXISTS (SELECT FROM vault.organization_members AS self
                       WHERE self.organization_id = tenant AND self.user_id = actor
                         AND self.role = 'owner' AND self.active) THEN
            RAISE EXCEPTION 'Organization owner required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF new_method NOT IN ('password', 'sso') THEN
            RAISE EXCEPTION 'Choose password or single sign-on' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        new_provider := nullif(btrim(lower(new_provider)), '');
        IF new_method = 'sso' AND new_provider IS NULL THEN
            RAISE EXCEPTION 'Enter the identity provider''s name' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        INSERT INTO vault.organization_sign_in (organization_id, method, provider, updated_at)
        VALUES (tenant, new_method, new_provider, now())
        ON CONFLICT (organization_id) DO UPDATE
            SET method = EXCLUDED.method, provider = EXCLUDED.provider, updated_at = now();

        -- Every member's login follows the organization's method. Leaving
        -- single sign-on gives them password sign-in back, but no password:
        -- an administrator sets new ones.
        FOR member_login IN
            SELECT principal.database_role_name
            FROM vault.organization_members AS member
            JOIN vault.users AS person ON person.id = member.user_id
            JOIN vault.manager_principals AS principal ON principal.user_id = person.id
            JOIN pg_roles AS login
              ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
            WHERE member.organization_id = tenant AND person.home_organization_id = tenant
              AND NOT vault.is_service_account(person.id)
        LOOP
            IF new_method = 'sso' THEN
                EXECUTE format('ALTER ROLE %I PASSWORD NULL', member_login);
            END IF;
            PERFORM vault.sync_password_sign_in(member_login, new_method = 'password');
        END LOOP;

        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, actor, 'organization.sign_in_changed',
                jsonb_build_object('method', new_method, 'provider', new_provider));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
REVOKE ALL ON FUNCTION vault.manager_set_sign_in(uuid, text, text) FROM PUBLIC;

CREATE FUNCTION vault.manager_set_sign_in_email(tenant uuid, member uuid, email text)
    RETURNS void
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        IF NOT has_function_privilege(session_user,
                'vault.manager_enroll_member(uuid,text,name,boolean,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        UPDATE vault.users SET sign_in_email = nullif(btrim(lower(email)), '')
        WHERE id = member AND home_organization_id = tenant AND NOT vault.is_service_account(id);
        IF NOT FOUND THEN
            RAISE EXCEPTION 'Select a member of this organization' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, vault.context_id('app.current_user_id'), 'organization.sign_in_email_changed',
                jsonb_build_object('user_id', member));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
REVOKE ALL ON FUNCTION vault.manager_set_sign_in_email(uuid, uuid, text) FROM PUBLIC;

-- ── Member administrators get the new entry points ───────────────────────
DO $migration$
DECLARE
    body text;
    login name;
BEGIN
    SELECT pg_get_functiondef('vault.grant_desktop_capability_privileges(name,text)'::regprocedure) INTO body;
    body := replace(body,
        '''vault.manager_set_member_access(uuid,uuid,boolean,boolean)''];',
        '''vault.manager_set_member_access(uuid,uuid,boolean,boolean)'',
                    ''vault.manager_sign_in(uuid)'',
                    ''vault.manager_set_sign_in(uuid,text,text)'',
                    ''vault.manager_set_sign_in_email(uuid,uuid,text)''];');
    IF body NOT LIKE '%manager_set_sign_in_email%' THEN
        RAISE EXCEPTION 'Could not update vault.grant_desktop_capability_privileges';
    END IF;
    EXECUTE body;

    FOR login IN
        SELECT role.rolname
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS role
          ON role.oid = principal.database_role_id AND role.rolname = principal.database_role_name
        WHERE has_function_privilege(role.oid, 'vault.manager_set_member_access(uuid,uuid,boolean,boolean)', 'EXECUTE')
    LOOP
        EXECUTE format('GRANT EXECUTE ON FUNCTION vault.manager_sign_in(uuid),
                            vault.manager_set_sign_in(uuid,text,text),
                            vault.manager_set_sign_in_email(uuid,uuid,text) TO %I', login);
    END LOOP;
END
$migration$;

-- ── Everyone signs in with a password today ──
-- Record that, so a server that later adds single sign-on to pg_hba.conf
-- keeps letting them in. If the role belongs to someone else and the owner
-- can't manage it, a server administrator grants it WITH ADMIN OPTION and
-- Manager's sign-in settings fill in the memberships.
DO $migration$
DECLARE
    login name;
BEGIN
    FOR login IN
        SELECT role.rolname
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS role
          ON role.oid = principal.database_role_id AND role.rolname = principal.database_role_name
        WHERE principal.active AND role.rolcanlogin
    LOOP
        PERFORM vault.sync_password_sign_in(login, true);
    END LOOP;
EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Password sign-in memberships were not recorded: %', SQLERRM;
END
$migration$;
