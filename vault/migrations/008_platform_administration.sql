-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- Platform administration separates three tiers:
--   * the schema owner, which runs migrations and server work;
--   * platform operators (the SaaS team), who oversee every organization;
--   * organization owners and admins, who manage only their own organization.
-- Each person belongs to exactly one organization. Platform operators belong
-- to none, and work inside an organization through its service account.

-- ── Platform identity ─────────────────────────────────────────────────────

-- The operator is derived from the authenticated login, never from settings
-- a SQL client could set.
CREATE FUNCTION vault.platform_operator_id() RETURNS uuid
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT operator.user_id
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login
          ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
        JOIN vault.platform_operators AS operator ON operator.user_id = principal.user_id
        WHERE login.rolname = session_user AND login.rolcanlogin
          AND principal.active AND operator.active
    $$;

REVOKE ALL ON FUNCTION vault.platform_operator_id() FROM PUBLIC;

-- True when a platform operator is working inside the given organization.
CREATE FUNCTION vault.is_platform_session(tenant uuid) RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT tenant = vault.context_id('app.current_organization_id')
           AND vault.platform_operator_id() IS NOT NULL
           AND vault.context_id('app.platform_actor_id') = vault.platform_operator_id()
    $$;

REVOKE ALL ON FUNCTION vault.is_platform_session(uuid) FROM PUBLIC;

-- ── Service accounts are managed by the platform ──────────────────────────

CREATE FUNCTION vault.is_service_account(member uuid) RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT EXISTS (SELECT FROM vault.organization_service_accounts AS service
                       WHERE service.user_id = member)
    $$;

REVOKE ALL ON FUNCTION vault.is_service_account(uuid) FROM PUBLIC;

CREATE FUNCTION vault.protect_service_membership() RETURNS trigger
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        IF vault.is_service_account(OLD.user_id) THEN
            RAISE EXCEPTION 'The service provider account is managed by the platform'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
        RETURN CASE WHEN TG_OP = 'DELETE' THEN OLD ELSE NEW END;
    END
    $$;

CREATE FUNCTION vault.reject_service_account_grant() RETURNS trigger
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        IF vault.is_service_account(NEW.user_id) THEN
            RAISE EXCEPTION 'The service provider account can''t be given groups, grants, or resources'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        RETURN NEW;
    END
    $$;

-- ── Creating organizations and operators ──────────────────────────────────

-- Internal: the organization row and its service account. Callers are the
-- schema owner or a platform function running as it.
CREATE FUNCTION vault.create_organization_record(
    tenant uuid, organization_name text, organization_time_zone text)
    RETURNS uuid
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        service uuid := gen_random_uuid();
    BEGIN
        IF EXISTS (SELECT FROM pg_roles WHERE rolname = current_user AND (rolsuper OR rolbypassrls)) THEN
            RAISE EXCEPTION 'Provision using the Vault schema owner subject to RLS'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF nullif(btrim(organization_name), '') IS NULL THEN
            RAISE EXCEPTION 'Enter an organization name' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF NOT EXISTS (SELECT FROM pg_timezone_names WHERE name = organization_time_zone) THEN
            RAISE EXCEPTION 'Unknown organization time zone' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        INSERT INTO vault.organizations (id, name, time_zone)
        VALUES (tenant, btrim(organization_name), organization_time_zone);
        INSERT INTO vault.users (id, issuer, subject, display_name, home_organization_id)
        VALUES (service, 'vault-service', tenant::text, 'Service provider', tenant);
        INSERT INTO vault.organization_members (organization_id, user_id, role, display_name)
        VALUES (tenant, service, 'owner', 'Service provider');
        INSERT INTO vault.organization_service_accounts (organization_id, user_id)
        VALUES (tenant, service);
        RETURN service;
    END
    $$;

REVOKE ALL ON FUNCTION vault.create_organization_record(uuid, text, text) FROM PUBLIC;

-- Internal: grants every Manager, ScheduleVault, and platform entry point.
-- Each entry point still checks the caller's organization and project access.
CREATE FUNCTION vault.grant_platform_operator_privileges(operator_login name)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        routine regprocedure;
    BEGIN
        EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', operator_login);
        FOR routine IN
            SELECT procedure.oid::regprocedure FROM pg_proc AS procedure
            WHERE procedure.pronamespace = 'vault'::regnamespace AND procedure.prosecdef
              AND procedure.proname ~ '^(manager|desktop|platform)_'
        LOOP
            EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO %I', routine, operator_login);
        END LOOP;
    END
    $$;

REVOKE ALL ON FUNCTION vault.grant_platform_operator_privileges(name) FROM PUBLIC;

-- Internal: registers a login as a platform operator.
CREATE FUNCTION vault.register_platform_operator(display_name text, operator_login name)
    RETURNS uuid
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        login_id oid;
        operator uuid := gen_random_uuid();
    BEGIN
        IF nullif(btrim(display_name), '') IS NULL THEN
            RAISE EXCEPTION 'Enter the administrator''s name' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        SELECT oid INTO login_id FROM pg_roles
        WHERE rolname = operator_login AND rolcanlogin AND NOT rolsuper
          AND NOT rolbypassrls AND NOT rolcreaterole AND NOT rolcreatedb
          AND NOT rolreplication AND NOT pg_has_role(oid, current_user, 'MEMBER');
        IF login_id IS NULL THEN
            RAISE EXCEPTION 'A separate unprivileged PostgreSQL login is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM pg_advisory_xact_lock(173195, (login_id::text::bigint % 2147483647)::integer);
        IF EXISTS (SELECT FROM vault.manager_principals AS principal WHERE principal.database_role_id = login_id) THEN
            RAISE EXCEPTION 'That login already belongs to a Vault user'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        INSERT INTO vault.users (id, issuer, subject, display_name, home_organization_id)
        VALUES (operator, 'postgresql', operator::text, btrim(display_name), NULL);
        INSERT INTO vault.manager_principals (database_role_id, database_role_name, user_id)
        VALUES (login_id, operator_login, operator);
        INSERT INTO vault.platform_operators (user_id) VALUES (operator);
        PERFORM vault.grant_platform_operator_privileges(operator_login);
        RETURN operator;
    END
    $$;

REVOKE ALL ON FUNCTION vault.register_platform_operator(text, name) FROM PUBLIC;

-- Schema-owner bootstrap, used when a Vault is created.
CREATE FUNCTION vault.provision_platform_operator(display_name text, operator_login name)
    RETURNS uuid
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        operator uuid;
    BEGIN
        IF NOT EXISTS (
            SELECT FROM pg_namespace AS schema
            JOIN pg_roles AS owner ON owner.oid = schema.nspowner
            WHERE schema.nspname = 'vault' AND owner.rolname = current_user
              AND NOT owner.rolsuper AND NOT owner.rolbypassrls
        ) THEN
            RAISE EXCEPTION 'Connect as the Vault schema owner' USING ERRCODE = 'insufficient_privilege';
        END IF;
        operator := vault.register_platform_operator(display_name, operator_login);
        INSERT INTO vault_admin.platform_events (operator_id, operator_login, operation, details)
        VALUES (NULL, session_user, 'platform.operator_added',
                jsonb_build_object('user_id', operator, 'login', operator_login));
        RETURN operator;
    END
    $$;

REVOKE ALL ON FUNCTION vault.provision_platform_operator(text, name) FROM PUBLIC;

-- ── Platform entry points ─────────────────────────────────────────────────

CREATE FUNCTION vault.require_platform_operator() RETURNS uuid
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        operator uuid := vault.platform_operator_id();
    BEGIN
        IF operator IS NULL THEN
            RAISE EXCEPTION 'Platform administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        RETURN operator;
    END
    $$;

REVOKE ALL ON FUNCTION vault.require_platform_operator() FROM PUBLIC;

CREATE FUNCTION vault.platform_list_organizations()
    RETURNS TABLE (organization_id uuid, organization_name text, organization_status text,
                   time_zone text, created_at timestamptz, member_count bigint,
                   project_count bigint, owner_names text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        previous_platform text := coalesce(current_setting('app.platform_actor_id', true), '');
        organization record;
    BEGIN
        PERFORM vault.require_platform_operator();
        FOR organization IN SELECT entry.id, entry.name, entry.status, entry.time_zone, entry.created_at
                            FROM vault.organizations AS entry ORDER BY lower(entry.name), entry.id
        LOOP
            PERFORM vault.bind_manager_context(organization.id);
            organization_id := organization.id;
            organization_name := organization.name;
            organization_status := organization.status;
            time_zone := organization.time_zone;
            created_at := organization.created_at;
            SELECT count(*) INTO member_count FROM vault.organization_members AS member
            WHERE member.organization_id = organization.id AND member.active
              AND NOT vault.is_service_account(member.user_id);
            SELECT count(*) INTO project_count FROM vault.projects AS project
            WHERE project.organization_id = organization.id AND NOT project.archived;
            SELECT string_agg(coalesce(member.display_name, identity.display_name), ', '
                              ORDER BY coalesce(member.display_name, identity.display_name))
            INTO owner_names
            FROM vault.organization_members AS member
            JOIN vault.users AS identity ON identity.id = member.user_id
            WHERE member.organization_id = organization.id AND member.role = 'owner' AND member.active
              AND NOT vault.is_service_account(member.user_id);
            RETURN NEXT;
        END LOOP;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        PERFORM set_config('app.platform_actor_id', previous_platform, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.platform_list_organizations() FROM PUBLIC;

CREATE FUNCTION vault.platform_create_organization(
    tenant uuid, organization_name text, organization_time_zone text)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        operator uuid := vault.require_platform_operator();
        service uuid;
    BEGIN
        service := vault.create_organization_record(tenant, organization_name, organization_time_zone);
        PERFORM set_config('app.platform_actor_id', operator::text, true);
        PERFORM set_config('app.current_organization_id', tenant::text, true);
        PERFORM set_config('app.current_user_id', service::text, true);
        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, service, 'organization.created', jsonb_build_object('name', btrim(organization_name)));
        INSERT INTO vault_admin.platform_events (operator_id, operator_login, organization_id, operation, details)
        VALUES (operator, session_user, tenant, 'platform.organization_created',
                jsonb_build_object('name', btrim(organization_name)));
        PERFORM set_config('app.current_organization_id', '', true);
        PERFORM set_config('app.current_user_id', '', true);
        PERFORM set_config('app.platform_actor_id', '', true);
        RETURN tenant;
    END
    $$;

REVOKE ALL ON FUNCTION vault.platform_create_organization(uuid, text, text) FROM PUBLIC;

CREATE FUNCTION vault.platform_set_organization_status(tenant uuid, new_status text)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        operator uuid := vault.require_platform_operator();
        changed boolean;
    BEGIN
        IF new_status NOT IN ('active', 'suspended') THEN
            RAISE EXCEPTION 'Unknown organization status' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        UPDATE vault.organizations AS organization SET status = new_status
        WHERE organization.id = tenant AND organization.status <> new_status;
        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, vault.context_id('app.current_user_id'),
                    CASE WHEN new_status = 'active' THEN 'organization.reactivated'
                         ELSE 'organization.suspended' END, '{}');
            INSERT INTO vault_admin.platform_events (operator_id, operator_login, organization_id, operation)
            VALUES (operator, session_user, tenant,
                    CASE WHEN new_status = 'active' THEN 'platform.organization_reactivated'
                         ELSE 'platform.organization_suspended' END);
        END IF;
        PERFORM set_config('app.current_organization_id', '', true);
        PERFORM set_config('app.current_user_id', '', true);
        PERFORM set_config('app.platform_actor_id', '', true);
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.platform_set_organization_status(uuid, text) FROM PUBLIC;

CREATE FUNCTION vault.platform_list_operators()
    RETURNS TABLE (user_id uuid, display_name text, login name, operator_active boolean)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        operator record;
    BEGIN
        PERFORM vault.require_platform_operator();
        FOR operator IN SELECT entry.user_id, entry.active FROM vault.platform_operators AS entry
        LOOP
            -- Read each identity through its self-read policy.
            PERFORM set_config('app.current_user_id', operator.user_id::text, true);
            user_id := operator.user_id;
            operator_active := operator.active;
            SELECT identity.display_name INTO display_name
            FROM vault.users AS identity WHERE identity.id = operator.user_id;
            SELECT principal.database_role_name INTO login
            FROM vault.manager_principals AS principal
            WHERE principal.user_id = operator.user_id AND principal.active
            ORDER BY principal.database_role_name LIMIT 1;
            RETURN NEXT;
        END LOOP;
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.platform_list_operators() FROM PUBLIC;

CREATE FUNCTION vault.platform_add_operator(display_name text, operator_login name)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        actor uuid := vault.require_platform_operator();
        operator uuid;
    BEGIN
        operator := vault.register_platform_operator(display_name, operator_login);
        INSERT INTO vault_admin.platform_events (operator_id, operator_login, operation, details)
        VALUES (actor, session_user, 'platform.operator_added',
                jsonb_build_object('user_id', operator, 'login', operator_login));
        RETURN operator;
    END
    $$;

REVOKE ALL ON FUNCTION vault.platform_add_operator(text, name) FROM PUBLIC;

CREATE FUNCTION vault.platform_set_operator_active(operator uuid, new_active boolean)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        actor uuid := vault.require_platform_operator();
        changed boolean;
    BEGIN
        IF operator = actor AND NOT new_active THEN
            RAISE EXCEPTION 'You can''t deactivate your own platform administrator account'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        UPDATE vault.platform_operators AS entry SET active = new_active
        WHERE entry.user_id = operator AND entry.active IS DISTINCT FROM new_active;
        changed := FOUND;
        IF changed THEN
            INSERT INTO vault_admin.platform_events (operator_id, operator_login, operation, details)
            VALUES (actor, session_user,
                    CASE WHEN new_active THEN 'platform.operator_reactivated'
                         ELSE 'platform.operator_deactivated' END,
                    jsonb_build_object('user_id', operator));
        END IF;
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.platform_set_operator_active(uuid, boolean) FROM PUBLIC;

CREATE FUNCTION vault.platform_list_events(requested_limit integer)
    RETURNS TABLE (recorded_at timestamptz, operator_login name, organization_id uuid,
                   operation text, details jsonb)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    BEGIN
        PERFORM vault.require_platform_operator();
        RETURN QUERY SELECT event.recorded_at, event.operator_login, event.organization_id,
                            event.operation, event.details
            FROM vault_admin.platform_events AS event
            ORDER BY event.recorded_at DESC, event.id DESC
            LIMIT greatest(1, least(coalesce(requested_limit, 200), 1000));
    END
    $$;

REVOKE ALL ON FUNCTION vault.platform_list_events(integer) FROM PUBLIC;

-- Reports whether the signed-in login is a platform operator.
CREATE FUNCTION vault.manager_session_profile()
    RETURNS TABLE (platform_admin boolean, display_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        operator uuid := vault.platform_operator_id();
    BEGIN
        platform_admin := operator IS NOT NULL;
        IF operator IS NOT NULL THEN
            PERFORM set_config('app.current_user_id', operator::text, true);
            SELECT identity.display_name INTO display_name FROM vault.users AS identity WHERE identity.id = operator;
            PERFORM set_config('app.current_user_id', previous_actor, true);
        END IF;
        RETURN NEXT;
    END
    $$;

GRANT EXECUTE ON FUNCTION vault.manager_session_profile() TO PUBLIC;

-- A login may be created without a password (single sign-on only).
CREATE FUNCTION vault.create_vault_login(desktop_login name, password_verifier text)
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

REVOKE ALL ON FUNCTION vault.create_vault_login(name, text) FROM PUBLIC;

-- Platform administrators always keep a password (the way in when the
-- sign-in service is down).
CREATE FUNCTION vault.platform_create_login(desktop_login name, password_verifier text)
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
        INSERT INTO vault_admin.platform_events (operator_id, operator_login, operation, details)
        VALUES (operator, session_user, 'platform.login_created', jsonb_build_object('login', desktop_login));
    END
    $$;

REVOKE ALL ON FUNCTION vault.platform_create_login(name, text) FROM PUBLIC;

-- Members of a password organization get a password; members of a single
-- sign-on organization must not.
CREATE FUNCTION vault.manager_create_login(tenant uuid, desktop_login name, password_verifier text)
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
                'vault.manager_enroll_member(uuid,text,name)', 'EXECUTE') THEN
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
        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, vault.context_id('app.current_user_id'), 'organization.login_created',
                jsonb_build_object('login', desktop_login, 'single_sign_on', sso));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_create_login(uuid, name, text) FROM PUBLIC;

-- Time entered by someone with Edit for another member. Anyone allowed
-- desktop_create_time may call it; the insert policy decides the rest.
CREATE FUNCTION vault.desktop_create_time_for(
    tenant uuid, project uuid, entry_id uuid, worker uuid, task uuid, assignment uuid,
    day date, millis bigint, notes text, is_billable boolean)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        IF NOT has_function_privilege(session_user,
                'vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Time entry permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        INSERT INTO vault.time_entries
            (organization_id, project_id, id, user_id, task_id, assignment_id,
             work_date, work_millis, comments, billable)
        VALUES (tenant, project, entry_id, coalesce(worker, vault.context_id('app.current_user_id')),
                task, assignment, day, millis, notes, is_billable);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN entry_id;
    END
    $$;

CREATE TRIGGER protect_service_membership
    BEFORE UPDATE OR DELETE ON vault.organization_members
    FOR EACH ROW EXECUTE FUNCTION vault.protect_service_membership();

CREATE TRIGGER reject_service_account_grant
    BEFORE INSERT OR UPDATE ON vault.group_members
    FOR EACH ROW EXECUTE FUNCTION vault.reject_service_account_grant();

CREATE TRIGGER reject_service_account_grant
    BEFORE INSERT OR UPDATE ON vault.project_members
    FOR EACH ROW EXECUTE FUNCTION vault.reject_service_account_grant();

CREATE TRIGGER reject_service_account_grant
    BEFORE INSERT OR UPDATE ON vault.resource_user_mappings
    FOR EACH ROW EXECUTE FUNCTION vault.reject_service_account_grant();

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY manage_platform_operators ON vault.platform_operators
         FOR ALL TO %I USING (true) WITH CHECK (true)', current_user);
    EXECUTE format(
        'CREATE POLICY manage_service_accounts ON vault.organization_service_accounts
         FOR ALL TO %I USING (true) WITH CHECK (true)', current_user);
    EXECUTE format(
        'CREATE POLICY platform_manage_organizations ON vault.organizations
         FOR ALL TO %I USING (vault.platform_operator_id() IS NOT NULL)
         WITH CHECK (vault.platform_operator_id() IS NOT NULL)', current_user);
    EXECUTE format(
        'CREATE POLICY platform_manage_members ON vault.organization_members
         FOR ALL TO %I USING (vault.is_platform_session(organization_id))
         WITH CHECK (vault.is_platform_session(organization_id))', current_user);
    EXECUTE format(
        'CREATE POLICY platform_read_member_identities ON vault.users
         FOR SELECT TO %I USING (
             home_organization_id IS NOT NULL AND vault.is_platform_session(home_organization_id))',
        current_user);
    EXECUTE format(
        'CREATE POLICY append_administration_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND vault.is_organization_admin(organization_id)
         )', current_user);
END
$migration$;


REVOKE ALL ON vault_admin.platform_events FROM PUBLIC;
