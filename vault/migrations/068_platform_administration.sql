-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- Platform administration separates three tiers:
--   * the schema owner, which runs migrations and server work;
--   * platform operators (the SaaS team), who oversee every organization;
--   * organization owners and admins, who manage only their own organization.
-- Each person belongs to exactly one organization. Platform operators belong
-- to none, and work inside an organization through its service account.

DO $migration$
BEGIN
    IF current_setting('server_version_num')::integer < 160000 THEN
        RAISE EXCEPTION 'Schedule Vault requires PostgreSQL 16 or later'
            USING ERRCODE = 'feature_not_supported';
    END IF;
END
$migration$;

-- ── Organizations and identities ──────────────────────────────────────────

ALTER TABLE vault.organizations
    ADD COLUMN status text NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'suspended')),
    ADD COLUMN created_at timestamptz NOT NULL DEFAULT transaction_timestamp();

-- A person's home organization. It is NULL only for platform operators.
ALTER TABLE vault.users
    ADD COLUMN home_organization_id uuid REFERENCES vault.organizations,
    ADD CONSTRAINT users_home_identity UNIQUE (home_organization_id, id);

-- A membership must be in the member's home organization, so nobody can be
-- added to a second organization.
ALTER TABLE vault.organization_members
    ADD CONSTRAINT organization_members_home_organization
    FOREIGN KEY (organization_id, user_id) REFERENCES vault.users (home_organization_id, id);

-- One service account per organization. Platform operators act through it, so
-- every organization policy applies to them without a personal membership.
CREATE TABLE vault.organization_service_accounts (
    organization_id uuid PRIMARY KEY REFERENCES vault.organizations,
    user_id uuid NOT NULL UNIQUE,
    FOREIGN KEY (organization_id, user_id) REFERENCES vault.organization_members
);

CREATE TABLE vault.platform_operators (
    user_id uuid PRIMARY KEY REFERENCES vault.users,
    active boolean NOT NULL DEFAULT true,
    created_at timestamptz NOT NULL DEFAULT transaction_timestamp()
);

ALTER TABLE vault.organization_service_accounts ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.organization_service_accounts FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.platform_operators ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.platform_operators FORCE ROW LEVEL SECURITY;

-- Platform activity that isn't tied to one organization's audit log.
CREATE TABLE vault_admin.platform_events (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    recorded_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    operator_id uuid,
    operator_login name NOT NULL,
    organization_id uuid,
    operation text NOT NULL CHECK (btrim(operation) <> ''),
    details jsonb NOT NULL DEFAULT '{}' CHECK (jsonb_typeof(details) = 'object')
);
REVOKE ALL ON vault_admin.platform_events FROM PUBLIC;

-- Organization audit entries name the platform operator behind a service
-- account action. Ordinary sessions reset the setting, so it stays NULL.
ALTER TABLE vault.audit_events
    ADD COLUMN platform_actor_id uuid REFERENCES vault.users
        DEFAULT vault.context_id('app.platform_actor_id');

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

CREATE TRIGGER protect_service_membership
    BEFORE UPDATE OR DELETE ON vault.organization_members
    FOR EACH ROW EXECUTE FUNCTION vault.protect_service_membership();

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

CREATE TRIGGER reject_service_account_grant
    BEFORE INSERT OR UPDATE ON vault.group_members
    FOR EACH ROW EXECUTE FUNCTION vault.reject_service_account_grant();
CREATE TRIGGER reject_service_account_grant
    BEFORE INSERT OR UPDATE ON vault.project_members
    FOR EACH ROW EXECUTE FUNCTION vault.reject_service_account_grant();
CREATE TRIGGER reject_service_account_grant
    BEFORE INSERT OR UPDATE ON vault.resource_user_mappings
    FOR EACH ROW EXECUTE FUNCTION vault.reject_service_account_grant();

-- ── Context binding ───────────────────────────────────────────────────────

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

-- Schema-owner provisioning of an organization with its owner, for dedicated
-- instances and tests. The owner's home is the new organization.
CREATE OR REPLACE FUNCTION vault.provision_organization(
    tenant uuid, organization_name text, organization_time_zone text,
    actor uuid, display_name text, desktop_login name,
    allow_initial_import boolean, allow_schedule_updates boolean)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        login_id oid;
    BEGIN
        IF EXISTS (SELECT FROM pg_roles WHERE rolname = current_user AND (rolsuper OR rolbypassrls)) THEN
            RAISE EXCEPTION 'Provision using the Vault schema owner subject to RLS'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
        SELECT oid INTO login_id FROM pg_roles
        WHERE rolname = desktop_login AND rolcanlogin AND NOT rolsuper
          AND NOT rolbypassrls AND NOT rolcreaterole AND NOT rolcreatedb
          AND NOT rolreplication AND NOT pg_has_role(oid, current_user, 'MEMBER');
        IF login_id IS NULL OR EXISTS (
            SELECT FROM vault.manager_principals AS principal WHERE principal.database_role_id = login_id
        ) THEN
            RAISE EXCEPTION 'A separate unprivileged PostgreSQL login is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF NOT (coalesce(allow_initial_import, false) OR coalesce(allow_schedule_updates, false)) THEN
            RAISE EXCEPTION 'Select a desktop capability' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        PERFORM vault.create_organization_record(tenant, organization_name, organization_time_zone);
        INSERT INTO vault.users (id, issuer, subject, display_name, home_organization_id)
        VALUES (actor, 'postgresql', actor::text, display_name, tenant);
        INSERT INTO vault.organization_members (organization_id, user_id, role)
        VALUES (tenant, actor, 'owner');
        INSERT INTO vault.manager_principals (database_role_id, database_role_name, user_id)
        VALUES (login_id, desktop_login, actor);

        EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', desktop_login);
        EXECUTE format(
            'GRANT EXECUTE ON FUNCTION vault.manager_lookup_import_source(uuid,text,text,text) TO %I',
            desktop_login);
        IF allow_initial_import THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.manager_import_initial_schedule(
                    uuid,uuid,text,text,text,text,bytea,bytea,jsonb,jsonb,jsonb) TO %I', desktop_login);
        END IF;
        IF allow_schedule_updates THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.desktop_read_schedule(uuid,uuid),
                    vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb),
                    vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)
                 TO %I', desktop_login);
        END IF;
    END
    $$;

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

REVOKE ALL ON FUNCTION vault.platform_list_organizations() FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.platform_create_organization(uuid, text, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.platform_set_organization_status(uuid, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.platform_list_operators() FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.platform_add_operator(text, name) FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.platform_set_operator_active(uuid, boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.platform_list_events(integer) FROM PUBLIC;

-- ── Organization directory ────────────────────────────────────────────────

CREATE OR REPLACE FUNCTION vault.manager_list_organizations()
    RETURNS TABLE (organization_id uuid, organization_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        tenant uuid;
    BEGIN
        IF vault.platform_operator_id() IS NOT NULL THEN
            RETURN QUERY SELECT organization.id, organization.name
                FROM vault.organizations AS organization ORDER BY lower(organization.name), organization.id;
            RETURN;
        END IF;

        -- Resolve the authenticated login and enter directory scope. Each name
        -- is then read under the ordinary single-organization policy.
        PERFORM vault.bind_manager_context(NULL);
        PERFORM set_config('app.current_organization_id', '', true);
        FOR tenant IN SELECT member.organization_id FROM vault.organization_members AS member
                      WHERE member.user_id = vault.context_id('app.current_user_id') AND member.active
                      ORDER BY member.organization_id
        LOOP
            PERFORM set_config('app.current_organization_id', tenant::text, true);
            RETURN QUERY SELECT organization.id, organization.name
                FROM vault.organizations AS organization
                WHERE organization.id = tenant AND organization.status = 'active';
        END LOOP;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

CREATE OR REPLACE FUNCTION vault.manager_overview_counts()
    RETURNS TABLE (organization_count bigint, project_count bigint, active_membership_count bigint)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        tenant uuid;
        visible_projects bigint;
        active_members bigint;
        can_list_projects boolean := has_function_privilege(session_user, 'vault.desktop_list_projects(uuid)', 'EXECUTE');
        can_list_members boolean := has_function_privilege(session_user,
            'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE');
    BEGIN
        organization_count := 0;
        IF can_list_projects THEN
            project_count := 0;
        END IF;
        FOR tenant IN SELECT directory.organization_id FROM vault.manager_list_organizations() AS directory
        LOOP
            organization_count := organization_count + 1;
            PERFORM vault.bind_manager_context(tenant);
            IF can_list_projects THEN
                SELECT count(*) INTO visible_projects
                FROM vault.projects AS entry WHERE entry.organization_id = tenant;
                project_count := project_count + visible_projects;
            END IF;
            IF can_list_members AND coalesce(vault.is_organization_admin(tenant), false) THEN
                SELECT count(*) INTO active_members FROM vault.organization_members AS member
                WHERE member.organization_id = tenant AND member.active
                  AND NOT vault.is_service_account(member.user_id);
                active_membership_count := coalesce(active_membership_count, 0) + active_members;
            END IF;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN NEXT;
    END
    $$;

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

-- ── Members stay in their home organization ───────────────────────────────

CREATE OR REPLACE FUNCTION vault.manager_enroll_member(
    tenant uuid, display_name text, desktop_login name, allow_schedule boolean, allow_time boolean)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        login_id oid;
        member_id uuid;
        home uuid;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        actor := vault.context_id('app.current_user_id');
        IF nullif(btrim(display_name), '') IS NULL
           OR NOT (coalesce(allow_schedule, false) OR coalesce(allow_time, false)) THEN
            RAISE EXCEPTION 'A member name and desktop capability are required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        SELECT oid INTO login_id FROM pg_roles
        WHERE rolname = desktop_login AND rolcanlogin AND NOT rolsuper
          AND NOT rolbypassrls AND NOT rolcreaterole AND NOT rolcreatedb
          AND NOT rolreplication AND NOT pg_has_role(oid, current_user, 'MEMBER');
        IF login_id IS NOT NULL THEN
            PERFORM pg_advisory_xact_lock(173195, (login_id::text::bigint % 2147483647)::integer);
            SELECT principal.user_id INTO member_id
            FROM vault.manager_principals AS principal
            WHERE principal.database_role_id = login_id AND principal.database_role_name = desktop_login;
            IF member_id IS NOT NULL THEN
                PERFORM set_config('app.current_user_id', member_id::text, true);
                SELECT identity.home_organization_id INTO home
                FROM vault.users AS identity WHERE identity.id = member_id;
                PERFORM set_config('app.current_user_id', actor::text, true);
                IF home IS DISTINCT FROM tenant
                   OR NOT EXISTS (SELECT FROM vault.manager_principals AS principal
                                  WHERE principal.database_role_id = login_id AND principal.active) THEN
                    login_id := NULL;
                END IF;
            END IF;
        END IF;
        -- A missing login, one registered to another organization, and a
        -- platform login are rejected at one place with one message, so the
        -- error reveals nothing about other tenants.
        IF login_id IS NULL THEN
            RAISE EXCEPTION 'That login isn''t available. Use a new login, or one that belongs to this organization.'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF member_id IS NULL THEN
            member_id := gen_random_uuid();
            INSERT INTO vault.users (id, issuer, subject, display_name, home_organization_id)
            VALUES (member_id, 'postgresql', member_id::text, display_name, tenant);
            INSERT INTO vault.manager_principals (database_role_id, database_role_name, user_id)
            VALUES (login_id, desktop_login, member_id);
        END IF;

        INSERT INTO vault.organization_members (organization_id, user_id, role)
        VALUES (tenant, member_id, 'member')
        ON CONFLICT DO NOTHING;
        IF FOUND THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, actor, 'organization.member_enrolled', jsonb_build_object('user_id', member_id));
        END IF;

        EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', desktop_login);
        IF allow_schedule THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.desktop_read_schedule(uuid,uuid),
                    vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb),
                    vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text),
                    vault.manager_lookup_import_source(uuid,text,text,text) TO %I', desktop_login);
        END IF;
        IF allow_time THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.desktop_list_time_work(uuid,uuid),
                    vault.desktop_list_time(uuid,uuid),
                    vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean),
                    vault.desktop_update_time(uuid,uuid,uuid,text,text,date,bigint,text,boolean)
                 TO %I', desktop_login);
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN member_id;
    END
    $$;

-- Internal: creates an unprivileged login from a SCRAM verifier. The schema
-- owner holds CREATEROLE, which PostgreSQL 16 limits to roles it creates.
-- Manager sends the verifier, so the password never appears in SQL or logs.
CREATE FUNCTION vault.create_vault_login(desktop_login name, password_verifier text)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        IF nullif(btrim(desktop_login), '') IS NULL OR desktop_login ~ '^pg_' THEN
            RAISE EXCEPTION 'Enter a valid login name' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF password_verifier IS NULL
           OR password_verifier !~ '^SCRAM-SHA-256\$[0-9]+:[A-Za-z0-9+/=]+\$[A-Za-z0-9+/=]+:[A-Za-z0-9+/=]+$' THEN
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
        EXECUTE format('CREATE ROLE %I LOGIN PASSWORD %L NOSUPERUSER NOBYPASSRLS NOCREATEDB '
                       'NOCREATEROLE NOREPLICATION', desktop_login, password_verifier);
    END
    $$;
REVOKE ALL ON FUNCTION vault.create_vault_login(name, text) FROM PUBLIC;

-- Platform administrators create logins for new platform administrators.
CREATE FUNCTION vault.platform_create_login(desktop_login name, password_verifier text)
    RETURNS void
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        operator uuid := vault.require_platform_operator();
    BEGIN
        PERFORM vault.create_vault_login(desktop_login, password_verifier);
        INSERT INTO vault_admin.platform_events (operator_id, operator_login, operation, details)
        VALUES (operator, session_user, 'platform.login_created', jsonb_build_object('login', desktop_login));
    END
    $$;
REVOKE ALL ON FUNCTION vault.platform_create_login(name, text) FROM PUBLIC;

-- Organization admins create logins for their own people.
CREATE FUNCTION vault.manager_create_login(tenant uuid, desktop_login name, password_verifier text)
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
        PERFORM vault.create_vault_login(desktop_login, password_verifier);
        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, vault.context_id('app.current_user_id'), 'organization.login_created',
                jsonb_build_object('login', desktop_login));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
REVOKE ALL ON FUNCTION vault.manager_create_login(uuid, name, text) FROM PUBLIC;

-- Organization admins turn on desktop features for their own members. A login
-- belongs to one organization, so the grant can't reach another tenant.
CREATE FUNCTION vault.manager_enable_member_capability(tenant uuid, member uuid, capability text)
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
        actor := vault.context_id('app.current_user_id');
        -- Only owners and the platform may hand out administration routines.
        IF capability = 'management' AND NOT EXISTS (
            SELECT FROM vault.organization_members AS self
            WHERE self.organization_id = tenant AND self.user_id = actor AND self.role = 'owner' AND self.active
        ) THEN
            RAISE EXCEPTION 'Organization owner required' USING ERRCODE = 'insufficient_privilege';
        END IF;

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
        IF home IS DISTINCT FROM tenant OR member_login IS NULL OR vault.is_service_account(member) THEN
            RAISE EXCEPTION 'Select an active member of this organization with a Vault login'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        PERFORM vault.enable_desktop_capability(member_login, capability);
        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, actor, 'organization.member_capability_enabled',
                jsonb_build_object('user_id', member, 'capability', capability));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
REVOKE ALL ON FUNCTION vault.manager_enable_member_capability(uuid, uuid, text) FROM PUBLIC;

-- Internal: an owner needs every Manager and ScheduleVault feature.
CREATE FUNCTION vault.grant_owner_capabilities(tenant uuid, owner_id uuid)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        owner_login name;
        capability text;
    BEGIN
        SELECT login.rolname INTO owner_login
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login
          ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
        WHERE principal.user_id = owner_id AND principal.active AND login.rolcanlogin
        ORDER BY login.rolname LIMIT 1;
        IF owner_login IS NULL THEN
            RETURN;
        END IF;
        FOREACH capability IN ARRAY ARRAY['directory', 'management', 'resources', 'audit', 'transfers',
                                          'export', 'time', 'schedule_updates']
        LOOP
            PERFORM vault.enable_desktop_capability(owner_login, capability);
        END LOOP;
        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, vault.context_id('app.current_user_id'), 'organization.member_capability_enabled',
                jsonb_build_object('user_id', owner_id, 'capability', 'owner'));
    END
    $$;
REVOKE ALL ON FUNCTION vault.grant_owner_capabilities(uuid, uuid) FROM PUBLIC;

-- Ownership: the platform can name any active member as owner, which is how a
-- new customer organization gets its first owner.
CREATE OR REPLACE FUNCTION vault.manager_transfer_ownership(tenant uuid, new_owner uuid)
    RETURNS void
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        changed integer;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM set_config('app.ownership_transfer_target', '', true);
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        PERFORM pg_advisory_xact_lock(hashtextextended(tenant::text, 38));

        IF vault.is_platform_session(tenant) THEN
            IF new_owner IS NULL OR vault.is_service_account(new_owner) OR NOT EXISTS (
                SELECT FROM vault.organization_members AS member
                WHERE member.organization_id = tenant AND member.user_id = new_owner AND member.active
            ) THEN
                RAISE EXCEPTION 'Ownership requires an active member in this organization'
                    USING ERRCODE = 'invalid_parameter_value';
            END IF;
            UPDATE vault.organization_members AS member SET role = 'admin'
            WHERE member.organization_id = tenant AND member.role = 'owner' AND member.user_id <> new_owner
              AND NOT vault.is_service_account(member.user_id);
            UPDATE vault.organization_members AS member SET role = 'owner'
            WHERE member.organization_id = tenant AND member.user_id = new_owner;
            PERFORM vault.grant_owner_capabilities(tenant, new_owner);
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, actor, 'organization.ownership_transferred', jsonb_build_object('new_owner', new_owner));
            PERFORM set_config('app.current_organization_id', previous_tenant, true);
            PERFORM set_config('app.current_user_id', previous_actor, true);
            RETURN;
        END IF;

        IF NOT EXISTS (
            SELECT FROM vault.organization_members AS member
            WHERE member.organization_id = tenant AND member.user_id = actor
              AND member.role = 'owner' AND member.active
        ) THEN
            RAISE EXCEPTION 'Organization owner required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        IF new_owner IS NULL OR new_owner = actor OR vault.is_service_account(new_owner) OR NOT EXISTS (
            SELECT FROM vault.manager_principals AS principal
            JOIN pg_roles AS login
              ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
            WHERE principal.user_id = new_owner AND principal.active AND login.rolcanlogin
        ) THEN
            RAISE EXCEPTION 'Select a different member with an active Vault login'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM set_config('app.current_user_id', new_owner::text, true);
        IF NOT EXISTS (SELECT FROM vault.users AS identity WHERE identity.id = new_owner AND identity.active) THEN
            RAISE EXCEPTION 'Select an active Vault user' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM set_config('app.current_user_id', actor::text, true);
        PERFORM set_config('app.ownership_transfer_target', new_owner::text, true);

        UPDATE vault.organization_members AS member
        SET role = CASE WHEN member.user_id = actor THEN 'admin' ELSE 'owner' END
        WHERE member.organization_id = tenant AND member.active
          AND ((member.user_id = actor AND member.role = 'owner')
               OR (member.user_id = new_owner AND member.role IN ('member', 'admin')));
        GET DIAGNOSTICS changed = ROW_COUNT;
        IF changed <> 2 THEN
            RAISE EXCEPTION 'Ownership requires an active member in this organization'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM vault.grant_owner_capabilities(tenant, new_owner);

        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, actor, 'organization.ownership_transferred',
                jsonb_build_object('new_owner', new_owner));
        PERFORM set_config('app.ownership_transfer_target', '', true);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

-- ── Directories hide the service account ──────────────────────────────────

CREATE OR REPLACE FUNCTION vault.manager_list_members(tenant uuid)
    RETURNS TABLE (member_id uuid, display_name text, membership_role text, membership_active boolean)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        membership record;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        actor := vault.context_id('app.current_user_id');

        FOR membership IN
            SELECT member.user_id, member.role, member.active, member.display_name
            FROM vault.organization_members AS member
            WHERE member.organization_id = tenant AND NOT vault.is_service_account(member.user_id)
            ORDER BY NOT member.active, member.role, member.user_id
        LOOP
            PERFORM set_config('app.current_user_id', membership.user_id::text, true);
            SELECT identity.display_name INTO display_name
            FROM vault.users AS identity WHERE identity.id = membership.user_id;
            PERFORM set_config('app.current_user_id', actor::text, true);

            member_id := membership.user_id;
            membership_role := membership.role;
            membership_active := membership.active AND display_name IS NOT NULL;
            display_name := coalesce(membership.display_name, display_name);
            RETURN NEXT;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

CREATE OR REPLACE FUNCTION vault.manager_list_mapping_users(tenant uuid, project uuid)
    RETURNS TABLE (user_id uuid, display_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        member record;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_resource_user(uuid,uuid,uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Resource mapping permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_write_schedule(tenant, project), false) THEN
            RAISE EXCEPTION 'Project unavailable for resource mapping' USING ERRCODE = 'insufficient_privilege';
        END IF;

        FOR member IN SELECT membership.user_id, membership.display_name
                      FROM vault.organization_members AS membership
                      WHERE membership.organization_id = tenant AND membership.active
                        AND NOT vault.is_service_account(membership.user_id)
        LOOP
            PERFORM set_config('app.current_user_id', member.user_id::text, true);
            RETURN QUERY SELECT identity.id, coalesce(member.display_name, identity.display_name)
                FROM vault.users AS identity WHERE identity.id = member.user_id AND identity.active;
        END LOOP;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

-- The security export carries people, not the platform's service account.
DO $migration$
DECLARE
    body text;
BEGIN
    SELECT pg_get_functiondef('vault.manager_export_security(uuid)'::regprocedure) INTO body;
    body := replace(body,
        'WHERE member.organization_id = tenant
            ORDER BY member.user_id',
        'WHERE member.organization_id = tenant AND NOT vault.is_service_account(member.user_id)
            ORDER BY member.user_id');
    IF body NOT LIKE '%is_service_account%' THEN
        RAISE EXCEPTION 'Could not update vault.manager_export_security';
    END IF;
    EXECUTE body;
END
$migration$;

-- ── Capability sets include the new organization entry points ─────────────

DO $migration$
DECLARE
    body text;
BEGIN
    SELECT pg_get_functiondef('vault.grant_desktop_capability_privileges(name,text)'::regprocedure) INTO body;
    body := replace(body,
        '''vault.manager_set_project_archived(uuid,uuid,boolean)''];',
        '''vault.manager_set_project_archived(uuid,uuid,boolean)'',
                    ''vault.manager_create_login(uuid,name,text)'',
                    ''vault.manager_enable_member_capability(uuid,uuid,text)''];');
    IF body NOT LIKE '%manager_create_login%' THEN
        RAISE EXCEPTION 'Could not update vault.grant_desktop_capability_privileges';
    END IF;
    EXECUTE body;
END
$migration$;
