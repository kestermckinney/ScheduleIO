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
-- Password logins without a recorded identity are unaffected.

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
-- Organizations that sign in with passwords keep ordinary logins with
-- passwords of their own.
--
-- The broker signs in with a login the Vault owner registers with
-- vault.add_sign_in_broker, which may run broker_issue_sign_in and nothing
-- else. Brokers belong to one Vault: a broker of another Vault on the same
-- server can't open this one's logins. It is trusted to have verified the
-- Supabase sign-in; the database decides which login, if any, that sign-in
-- may open:
--   * a person already linked to the Supabase account (issuer and subject),
--     or else the person whose sign-in email matches;
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

-- Signing in with an email. PostgreSQL needs a login name before anyone can
-- connect, so the apps first connect as this Vault's lookup login,
-- "<database>_lookup", whose password is its own name. It may only run
-- vault.login_for_email(), which returns the login of the active person with
-- that sign-in email (or nothing), and then sign in as that person with
-- their own password. Anyone with the lookup login can learn whether an
-- email has a login here; it opens nothing else.

CREATE FUNCTION vault.organization_uses_sso(tenant uuid)
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT coalesce((SELECT setting.method = 'sso' FROM vault.organization_sign_in AS setting
                         WHERE setting.organization_id = tenant), false)
    $$;

REVOKE ALL ON FUNCTION vault.organization_uses_sso(uuid) FROM PUBLIC;

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
                   'directory', setting.directory,
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

-- An organization administrator sets a new password for one of their own
-- people. Manager sends only a SCRAM verifier, so the password never reaches
-- the database. Organizations that sign in with single sign-on don't use
-- passwords, and platform administrators keep their own.

CREATE FUNCTION vault.manager_set_member_password(tenant uuid, member uuid, password_verifier text)
    RETURNS void
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        home uuid;
        member_login name;
    BEGIN
        IF NOT has_function_privilege(session_user,
                'vault.manager_enroll_member(uuid,text,name,boolean,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF password_verifier IS NULL
           OR password_verifier !~ '^SCRAM-SHA-256\$[0-9]+:[A-Za-z0-9+/=]+\$[A-Za-z0-9+/=]+:[A-Za-z0-9+/=]+$' THEN
            RAISE EXCEPTION 'A SCRAM-SHA-256 password verifier is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF EXISTS (SELECT FROM vault.organization_sign_in AS setting
                   WHERE setting.organization_id = tenant AND setting.method = 'sso') THEN
            RAISE EXCEPTION 'People in this organization sign in with single sign-on, not passwords'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        actor := vault.context_id('app.current_user_id');
        PERFORM set_config('app.current_user_id', member::text, true);
        SELECT identity.home_organization_id INTO home
        FROM vault.users AS identity WHERE identity.id = member AND identity.active;
        PERFORM set_config('app.current_user_id', actor::text, true);
        SELECT login.rolname INTO member_login
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login
          ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
        WHERE principal.user_id = member AND principal.active AND login.rolcanlogin
        ORDER BY login.rolname LIMIT 1;
        IF home IS DISTINCT FROM tenant OR member_login IS NULL OR vault.is_service_account(member)
           OR EXISTS (SELECT FROM vault.platform_operators AS operator WHERE operator.user_id = member) THEN
            RAISE EXCEPTION 'Select an active member of this organization with a Vault login'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        EXECUTE format('ALTER ROLE %I PASSWORD %L VALID UNTIL %L', member_login, password_verifier, 'infinity');
        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, actor, 'organization.member_password_set',
                jsonb_build_object('user_id', member, 'login', member_login));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_set_member_password(uuid, uuid, text) FROM PUBLIC;

CREATE FUNCTION vault.login_for_email(email text)
    RETURNS name
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        wanted text := nullif(btrim(lower(email)), '');
        found name;
        matches integer;
    BEGIN
        IF wanted IS NULL THEN
            RETURN NULL;
        END IF;
        PERFORM set_config('app.login_lookup', 'on', true);
        SELECT min(principal.database_role_name), count(DISTINCT principal.database_role_name)
        INTO found, matches
        FROM vault.users AS account
        JOIN vault.manager_principals AS principal ON principal.user_id = account.id AND principal.active
        JOIN pg_roles AS login
          ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
        WHERE account.sign_in_email = wanted AND account.active AND login.rolcanlogin
          AND NOT vault.is_service_account(account.id);
        PERFORM set_config('app.login_lookup', '', true);
        -- Two people sharing an email can't be told apart; sign in with the login.
        RETURN CASE WHEN matches = 1 THEN found END;
    END
    $$;

REVOKE ALL ON FUNCTION vault.login_for_email(text) FROM PUBLIC;

-- True only inside login_for_email, in a session of the lookup login.
CREATE FUNCTION vault.in_login_lookup()
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT coalesce(current_setting('app.login_lookup', true), '') = 'on'
           AND session_user = (current_database() || '_lookup')::name
    $$;

REVOKE ALL ON FUNCTION vault.in_login_lookup() FROM PUBLIC;

DO $migration$
BEGIN
    -- Only bind_manager_context, inside a token sign-in, may record an identity.
    EXECUTE format(
        'CREATE POLICY link_sign_in_identity ON vault.users FOR UPDATE TO %I
         USING (id = vault.context_id(''app.current_user_id'')
                AND issuer !~ ''^https?://''
                AND current_setting(''app.link_sign_in_identity'', true) = ''on'')
         WITH CHECK (id = vault.context_id(''app.current_user_id''))', current_user);
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
    -- ── Single sign-on logins refuse lasting passwords ──
    -- People linked to single sign-on, or in a single sign-on organization, may
    -- only sign in with a password from the broker, which always expires.
    --
    -- bind_manager_context checks this before it sets the organization, so a
    -- session may read how its own home organization signs in.
    EXECUTE format(
        'CREATE POLICY read_home_sign_in ON vault.organization_sign_in FOR SELECT TO %I
         USING (organization_id = (SELECT account.home_organization_id FROM vault.users AS account
                                   WHERE account.id = vault.context_id(''app.current_user_id'')))', current_user);
    EXECUTE format('CREATE POLICY lookup_read_people ON vault.users FOR SELECT TO %I
                    USING (vault.in_login_lookup())', current_user);
    EXECUTE format('CREATE POLICY lookup_read_principals ON vault.manager_principals FOR SELECT TO %I
                    USING (vault.in_login_lookup())', current_user);
END
$migration$;

-- The lookup login. Creating it needs CREATEROLE, which the Vault owner has
-- when Schedule Vault Manager made the Vault; otherwise a server administrator
-- can create it and run the grants below.
DO $migration$
DECLARE
    lookup name := (current_database() || '_lookup')::name;
BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = lookup) THEN
        IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = current_user AND rolcreaterole) THEN
            RAISE NOTICE 'Signing in with an email needs the login %; the Vault owner can''t create it', lookup;
            RETURN;
        END IF;
        EXECUTE format('CREATE ROLE %I LOGIN PASSWORD %L NOSUPERUSER NOBYPASSRLS NOCREATEDB '
                       'NOCREATEROLE NOREPLICATION', lookup, lookup);
    END IF;
    EXECUTE format('GRANT CONNECT ON DATABASE %I TO %I', current_database(), lookup);
    EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', lookup);
    EXECUTE format('GRANT EXECUTE ON FUNCTION vault.login_for_email(text) TO %I', lookup);
END
$migration$;
