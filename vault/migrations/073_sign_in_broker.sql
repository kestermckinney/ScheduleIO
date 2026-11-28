-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- Single sign-on through a credential broker.
--
-- A hosted PostgreSQL (Supabase) can't load an OAuth validator or change
-- pg_hba.conf, so a token can't open a login directly. Instead, people of a
-- single sign-on organization sign in to Supabase Auth with their
-- organization's identity provider (Microsoft Entra, Google Workspace, or
-- another OpenID Connect provider), and a small web service -- the broker --
-- checks that sign-in and asks this database for a short-lived password:
--
--   vault.broker_issue_sign_in(...) sets a password on the person's login
--   that works for five minutes (VALID UNTIL), and returns the login's name.
--   The broker sends only a SCRAM verifier; the password itself never
--   reaches the database. A session opened with it stays open after it
--   expires; the next connection needs a new one.
--
-- Organizations that sign in with passwords are unchanged: their people keep
-- ordinary logins with passwords of their own.
--
-- The broker signs in with a login the Vault owner registers with
-- vault.add_sign_in_broker, which may run broker_issue_sign_in and nothing
-- else. Brokers belong to one Vault: a broker of another Vault on the same
-- server can't open this one's logins. It is trusted to have verified the
-- Supabase sign-in; the database decides which login, if any, that sign-in
-- may open:
--   * a person already linked to the Supabase account (issuer and subject,
--     see migration 071), or else the person whose sign-in email matches;
--   * whose home organization signs in with single sign-on through the same
--     provider and directory (Entra tenant, Workspace domain, or issuer) the
--     account signed in with;
--   * who is active, with an active login, and is not a service account or
--     platform operator.
-- Only such logins are ever given a password, and only one that expires.
--
-- Such a person's login refuses a lasting password (one without VALID UNTIL):
-- bind_manager_context stops it, so a password set by hand doesn't get around
-- single sign-on.
--
-- The pg_hba.conf sign-in groups of migration 072 aren't used here.

-- ── Settings ──
-- provider: the Supabase Auth provider ("azure", "google", "keycloak", ...).
-- directory: what identifies the organization's accounts at that provider:
-- the Entra tenant ID for azure, the Workspace domain for google, and the
-- identity provider's issuer URL otherwise.
ALTER TABLE vault.organization_sign_in
    ADD COLUMN directory text CHECK (directory IS NULL OR (btrim(directory) <> '' AND length(directory) <= 255));
UPDATE vault.organization_sign_in SET method = 'password', provider = NULL WHERE method = 'sso';
ALTER TABLE vault.organization_sign_in
    ADD CHECK (method = 'password' OR directory IS NOT NULL);

-- pg_hba.conf groups aren't used: membership is no longer recorded.
CREATE OR REPLACE FUNCTION vault.sync_password_sign_in(desktop_login name, allowed boolean)
    RETURNS void
    LANGUAGE sql
    SET search_path = pg_catalog
    AS $$ SELECT $$;

-- ── Broker logins ──
-- Only the owner's functions read this table; no one else is granted it.
CREATE TABLE vault.sign_in_brokers (
    database_role_id oid PRIMARY KEY,
    database_role_name name NOT NULL UNIQUE,
    added_at timestamptz NOT NULL DEFAULT now()
);

CREATE FUNCTION vault.is_sign_in_broker(login name)
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT EXISTS (SELECT FROM vault.sign_in_brokers AS broker
                       JOIN pg_roles AS role
                         ON role.oid = broker.database_role_id AND role.rolname = broker.database_role_name
                       WHERE role.rolname = login AND role.rolcanlogin)
    $$;
REVOKE ALL ON FUNCTION vault.is_sign_in_broker(name) FROM PUBLIC;

-- True only inside broker_issue_sign_in, in a session of the broker.
CREATE FUNCTION vault.in_sign_in_broker()
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT coalesce(current_setting('app.sign_in_broker', true), '') = 'on'
           AND vault.is_sign_in_broker(session_user)
    $$;
REVOKE ALL ON FUNCTION vault.in_sign_in_broker() FROM PUBLIC;

DO $migration$
BEGIN
    EXECUTE format('CREATE POLICY broker_read_people ON vault.users FOR SELECT TO %I
                    USING (vault.in_sign_in_broker())', current_user);
    EXECUTE format('CREATE POLICY broker_link_people ON vault.users FOR UPDATE TO %I
                    USING (vault.in_sign_in_broker() AND issuer !~ ''^https?://'')
                    WITH CHECK (vault.in_sign_in_broker())', current_user);
    EXECUTE format('CREATE POLICY broker_read_members ON vault.organization_members FOR SELECT TO %I
                    USING (vault.in_sign_in_broker())', current_user);
    EXECUTE format('CREATE POLICY broker_read_principals ON vault.manager_principals FOR SELECT TO %I
                    USING (vault.in_sign_in_broker())', current_user);
    EXECUTE format('CREATE POLICY broker_read_organizations ON vault.organizations FOR SELECT TO %I
                    USING (vault.in_sign_in_broker())', current_user);
    EXECUTE format('CREATE POLICY broker_read_sign_in ON vault.organization_sign_in FOR SELECT TO %I
                    USING (vault.in_sign_in_broker())', current_user);
    EXECUTE format('CREATE POLICY broker_audit ON vault.audit_events FOR ALL TO %I
                    USING (vault.in_sign_in_broker() AND operation = ''sign_in.credential_issued'')
                    WITH CHECK (vault.in_sign_in_broker() AND operation = ''sign_in.credential_issued'')',
                   current_user);
END
$migration$;

-- ── Issuing a credential ──
CREATE FUNCTION vault.broker_issue_sign_in(
        auth_issuer text, auth_subject text, account_email text,
        account_provider text, account_directory text, password_verifier text)
    RETURNS TABLE (login name, valid_until timestamptz)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        person uuid;
        tenant uuid;
        member_login name;
        login_role pg_roles%ROWTYPE;
        expires timestamptz := date_trunc('second', clock_timestamp()) + interval '5 minutes';
        refused constant text := 'This sign-in doesn''t open a Vault login';
    BEGIN
        IF NOT vault.is_sign_in_broker(session_user) THEN
            RAISE EXCEPTION 'Sign-in broker required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        auth_issuer := nullif(btrim(auth_issuer), '');
        auth_subject := nullif(btrim(auth_subject), '');
        account_email := nullif(btrim(lower(account_email)), '');
        account_provider := nullif(btrim(lower(account_provider)), '');
        account_directory := nullif(btrim(lower(account_directory)), '');
        IF auth_issuer IS NULL OR auth_issuer !~ '^https://' OR auth_subject IS NULL
           OR account_provider IS NULL OR account_directory IS NULL THEN
            RAISE EXCEPTION 'The sign-in''s issuer, subject, provider, and directory are required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF password_verifier IS NULL
           OR password_verifier !~ '^SCRAM-SHA-256\$[0-9]+:[A-Za-z0-9+/=]+\$[A-Za-z0-9+/=]+:[A-Za-z0-9+/=]+$' THEN
            RAISE EXCEPTION 'A SCRAM-SHA-256 password verifier is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        PERFORM set_config('app.sign_in_broker', 'on', true);

        -- The person: already linked to this account, or matched by email.
        SELECT account.id INTO person
        FROM vault.users AS account
        WHERE account.issuer = auth_issuer AND account.subject = auth_subject;
        IF person IS NULL AND account_email IS NOT NULL THEN
            SELECT account.id INTO person
            FROM vault.users AS account
            WHERE account.sign_in_email = account_email AND account.issuer !~ '^https?://';
        END IF;

        -- ...whose organization signs in through this provider and directory.
        SELECT account.home_organization_id, principal.database_role_name INTO tenant, member_login
        FROM vault.users AS account
        JOIN vault.organizations AS organization ON organization.id = account.home_organization_id
        JOIN vault.organization_sign_in AS setting ON setting.organization_id = organization.id
        JOIN vault.organization_members AS member
          ON member.organization_id = organization.id AND member.user_id = account.id
        JOIN vault.manager_principals AS principal ON principal.user_id = account.id AND principal.active
        WHERE account.id = person AND account.active AND member.active
          AND organization.status = 'active'
          AND setting.method = 'sso' AND setting.provider = account_provider AND lower(setting.directory) = account_directory
          AND NOT vault.is_service_account(account.id)
          AND NOT EXISTS (SELECT FROM vault.platform_operators AS operator WHERE operator.user_id = account.id);
        IF member_login IS NULL THEN
            RAISE EXCEPTION '%', refused USING ERRCODE = 'invalid_authorization_specification';
        END IF;

        SELECT role.* INTO login_role FROM pg_roles AS role
        JOIN vault.manager_principals AS principal
          ON principal.database_role_id = role.oid AND principal.database_role_name = role.rolname
        WHERE role.rolname = member_login;
        IF NOT login_role.rolcanlogin OR login_role.rolsuper OR login_role.rolcreaterole
           OR login_role.rolbypassrls OR login_role.rolreplication THEN
            RAISE EXCEPTION '%', refused USING ERRCODE = 'invalid_authorization_specification';
        END IF;

        -- At most six credentials a minute for one person.
        IF (SELECT count(*) FROM vault.audit_events AS event
            WHERE event.organization_id = tenant AND event.actor_id = person
              AND event.operation = 'sign_in.credential_issued'
              AND event.occurred_at > now() - interval '1 minute') >= 6 THEN
            RAISE EXCEPTION 'Too many sign-ins. Wait a minute and try again.'
                USING ERRCODE = 'program_limit_exceeded';
        END IF;

        -- The first sign-in links the person to the account.
        BEGIN
            UPDATE vault.users SET issuer = auth_issuer, subject = auth_subject
            WHERE id = person AND issuer !~ '^https?://';
        EXCEPTION WHEN unique_violation THEN
            RAISE EXCEPTION '%', refused USING ERRCODE = 'invalid_authorization_specification';
        END;

        EXECUTE format('ALTER ROLE %I PASSWORD %L VALID UNTIL %L', member_login, password_verifier, expires::text);
        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, person, 'sign_in.credential_issued',
                jsonb_build_object('login', member_login, 'provider', account_provider, 'valid_until', expires));
        PERFORM set_config('app.sign_in_broker', '', true);
        login := member_login;
        valid_until := expires;
        RETURN NEXT;
    END
    $$;
REVOKE ALL ON FUNCTION vault.broker_issue_sign_in(text, text, text, text, text, text) FROM PUBLIC;

-- The owner registers a broker's login, which must be a plain login of its
-- own (not a Vault person's, and without privileges of its own).
CREATE FUNCTION vault.add_sign_in_broker(login name)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        login_id oid;
    BEGIN
        SELECT oid INTO login_id FROM pg_roles
        WHERE rolname = login AND rolcanlogin AND NOT rolsuper AND NOT rolbypassrls
          AND NOT rolcreaterole AND NOT rolcreatedb AND NOT rolreplication
          AND NOT pg_has_role(oid, current_user, 'MEMBER');
        IF login_id IS NULL
           OR EXISTS (SELECT FROM vault.manager_principals AS principal WHERE principal.database_role_id = login_id) THEN
            RAISE EXCEPTION 'A separate unprivileged PostgreSQL login is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        INSERT INTO vault.sign_in_brokers (database_role_id, database_role_name)
        VALUES (login_id, login) ON CONFLICT DO NOTHING;
        EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', login);
        EXECUTE format('GRANT EXECUTE ON FUNCTION vault.broker_issue_sign_in(text,text,text,text,text,text) TO %I',
                       login);
    END
    $$;
REVOKE ALL ON FUNCTION vault.add_sign_in_broker(name) FROM PUBLIC;

CREATE FUNCTION vault.remove_sign_in_broker(login name)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        DELETE FROM vault.sign_in_brokers WHERE database_role_name = login;
        IF EXISTS (SELECT FROM pg_roles WHERE rolname = login) THEN
            EXECUTE format('REVOKE EXECUTE ON FUNCTION vault.broker_issue_sign_in(text,text,text,text,text,text) FROM %I',
                           login);
        END IF;
    END
    $$;
REVOKE ALL ON FUNCTION vault.remove_sign_in_broker(name) FROM PUBLIC;

-- ── Single sign-on logins refuse lasting passwords ──
-- People linked to single sign-on, or in a single sign-on organization, may
-- only sign in with a password from the broker, which always expires.
--
-- bind_manager_context checks this before it sets the organization, so a
-- session may read how its own home organization signs in.
DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY read_home_sign_in ON vault.organization_sign_in FOR SELECT TO %I
         USING (organization_id = (SELECT account.home_organization_id FROM vault.users AS account
                                   WHERE account.id = vault.context_id(''app.current_user_id'')))', current_user);
END
$migration$;
CREATE FUNCTION vault.lasting_password_refused(person uuid, login name)
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT (SELECT account.issuer ~ '^https?://' OR vault.organization_uses_sso(account.home_organization_id)
                FROM vault.users AS account WHERE account.id = person)
           AND (SELECT role.rolvaliduntil IS NULL OR role.rolvaliduntil = 'infinity'
                FROM pg_roles AS role WHERE role.rolname = login)
    $$;
REVOKE ALL ON FUNCTION vault.lasting_password_refused(uuid, name) FROM PUBLIC;

DO $migration$
DECLARE
    body text;
    old_check constant text := $old$        ELSIF person_issuer ~ '^https?://' THEN
            RAISE EXCEPTION 'This person signs in with single sign-on'$old$;
BEGIN
    SELECT pg_get_functiondef('vault.bind_manager_context(uuid)'::regprocedure) INTO body;
    IF position(old_check IN body) = 0 THEN
        RAISE EXCEPTION 'Could not update vault.bind_manager_context';
    END IF;
    body := replace(body, old_check, $new$        ELSIF vault.lasting_password_refused(actor, session_user) THEN
            RAISE EXCEPTION 'This person signs in with single sign-on'$new$);
    EXECUTE body;
END
$migration$;

-- ── Manager: provider and directory ──
DROP FUNCTION vault.manager_set_sign_in(uuid, text, text);

CREATE FUNCTION vault.manager_set_sign_in(tenant uuid, new_method text, new_provider text, new_directory text)
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
        new_directory := nullif(btrim(new_directory), '');
        IF new_method = 'sso' AND (new_provider IS NULL OR new_directory IS NULL) THEN
            RAISE EXCEPTION 'Choose the identity provider and enter the organization''s directory'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF new_method = 'password' THEN
            new_provider := NULL;
            new_directory := NULL;
        END IF;

        INSERT INTO vault.organization_sign_in (organization_id, method, provider, directory, updated_at)
        VALUES (tenant, new_method, new_provider, new_directory, now())
        ON CONFLICT (organization_id) DO UPDATE
            SET method = EXCLUDED.method, provider = EXCLUDED.provider,
                directory = EXCLUDED.directory, updated_at = now();

        -- Moving to single sign-on removes members' own passwords. Leaving it
        -- doesn't give them back: an administrator sets new ones.
        IF new_method = 'sso' THEN
            FOR member_login IN
                SELECT principal.database_role_name
                FROM vault.organization_members AS member
                JOIN vault.users AS person ON person.id = member.user_id
                JOIN vault.manager_principals AS principal ON principal.user_id = person.id
                JOIN pg_roles AS login
                  ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
                WHERE member.organization_id = tenant AND person.home_organization_id = tenant
                  AND NOT vault.is_service_account(person.id)
                  AND NOT EXISTS (SELECT FROM vault.platform_operators AS operator
                                  WHERE operator.user_id = person.id)
            LOOP
                EXECUTE format('ALTER ROLE %I PASSWORD NULL VALID UNTIL %L', member_login, '-infinity');
            END LOOP;
        END IF;

        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, actor, 'organization.sign_in_changed',
                jsonb_build_object('method', new_method, 'provider', new_provider, 'directory', new_directory));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
REVOKE ALL ON FUNCTION vault.manager_set_sign_in(uuid, text, text, text) FROM PUBLIC;

-- manager_sign_in also reports the directory.
DO $migration$
DECLARE
    body text;
BEGIN
    SELECT pg_get_functiondef('vault.manager_sign_in(uuid)'::regprocedure) INTO body;
    body := replace(body, $old$'provider', setting.provider,$old$,
                          $new$'provider', setting.provider,
                   'directory', setting.directory,$new$);
    IF body NOT LIKE '%''directory'', setting.directory%' THEN
        RAISE EXCEPTION 'Could not update vault.manager_sign_in';
    END IF;
    EXECUTE body;
END
$migration$;

-- Member administrators get the new entry point.
DO $migration$
DECLARE
    body text;
    login name;
BEGIN
    SELECT pg_get_functiondef('vault.grant_desktop_capability_privileges(name,text)'::regprocedure) INTO body;
    body := replace(body, '''vault.manager_set_sign_in(uuid,text,text)''',
                          '''vault.manager_set_sign_in(uuid,text,text,text)''');
    IF body NOT LIKE '%manager_set_sign_in(uuid,text,text,text)%' THEN
        RAISE EXCEPTION 'Could not update vault.grant_desktop_capability_privileges';
    END IF;
    EXECUTE body;

    FOR login IN
        SELECT role.rolname
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS role
          ON role.oid = principal.database_role_id AND role.rolname = principal.database_role_name
        WHERE has_function_privilege(role.oid, 'vault.manager_sign_in(uuid)', 'EXECUTE')
    LOOP
        EXECUTE format('GRANT EXECUTE ON FUNCTION vault.manager_set_sign_in(uuid,text,text,text) TO %I', login);
    END LOOP;
END
$migration$;
