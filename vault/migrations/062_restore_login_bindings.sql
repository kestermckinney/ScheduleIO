-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE vault_admin.desktop_capability_events DROP CONSTRAINT desktop_capability_events_action_check;
ALTER TABLE vault_admin.desktop_capability_events ADD CONSTRAINT desktop_capability_events_action_check
    CHECK (action IN ('enabled', 'revoked', 'rebound'));

CREATE FUNCTION vault.rebind_restored_desktop_logins(login_names text[])
    RETURNS integer
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        bindings jsonb;
        changed integer;
    BEGIN
        IF NOT EXISTS (
            SELECT FROM pg_namespace AS schema
            JOIN pg_roles AS owner ON owner.oid = schema.nspowner
            WHERE schema.nspname = 'vault' AND owner.rolname = current_user
              AND NOT owner.rolsuper AND NOT owner.rolbypassrls
        ) THEN
            RAISE EXCEPTION 'Connect as the Vault schema owner' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF coalesce(cardinality(login_names), 0) = 0 OR EXISTS (
            SELECT FROM unnest(login_names) AS requested(name) WHERE nullif(btrim(requested.name), '') IS NULL
        ) THEN
            RAISE EXCEPTION 'Enter the registered logins to rebind' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        LOCK TABLE vault.manager_principals IN SHARE ROW EXCLUSIVE MODE;
        SELECT jsonb_agg(jsonb_build_object(
                   'database_role_id', login.oid::text, 'database_role_name', login.rolname::text,
                   'user_id', principal.user_id, 'active', principal.active,
                   'requires_change', login.oid <> principal.database_role_id))
        INTO bindings
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login ON login.rolname = principal.database_role_name
        WHERE login.rolname::text = ANY(login_names) AND login.rolcanlogin
          AND NOT login.rolsuper AND NOT login.rolbypassrls AND NOT login.rolcreaterole
          AND NOT login.rolcreatedb AND NOT login.rolreplication
          AND NOT pg_has_role(login.oid, current_user, 'MEMBER');

        IF coalesce(jsonb_array_length(bindings), 0)
            <> (SELECT count(DISTINCT requested.name) FROM unnest(login_names) AS requested(name)) THEN
            RAISE EXCEPTION 'Every selected login must be registered in this Vault and exist as an unprivileged login'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        IF EXISTS (
            SELECT FROM jsonb_to_recordset(bindings) AS next(database_role_id oid)
            JOIN vault.manager_principals AS prior ON prior.database_role_id = next.database_role_id
            WHERE prior.database_role_name::text <> ALL(login_names)
        ) THEN
            RAISE EXCEPTION 'A destination role ID is still bound to another login. Rebind the affected logins together.'
                USING ERRCODE = 'unique_violation';
        END IF;

        -- Remove all obsolete keys before inserting replacements, including
        -- when restored role IDs are swapped between two selected logins.
        DELETE FROM vault.manager_principals AS principal
        USING jsonb_to_recordset(bindings) AS item(database_role_name name, requires_change boolean)
        WHERE principal.database_role_name = item.database_role_name AND item.requires_change;

        INSERT INTO vault.manager_principals (database_role_id, database_role_name, user_id, active)
        SELECT item.database_role_id, item.database_role_name, item.user_id, item.active
        FROM jsonb_to_recordset(bindings) AS item(
            database_role_id oid, database_role_name name, user_id uuid, active boolean, requires_change boolean)
        WHERE item.requires_change;
        GET DIAGNOSTICS changed = ROW_COUNT;

        INSERT INTO vault_admin.desktop_capability_events
            (operator_login, operator_role, target_login, capability, action)
        SELECT session_user, current_user, item.database_role_name, 'desktop_login', 'rebound'
        FROM jsonb_to_recordset(bindings) AS item(database_role_name name, requires_change boolean)
        WHERE item.requires_change;
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.rebind_restored_desktop_logins(text[]) FROM PUBLIC;

-- Identity subjects are stable Vault identifiers. The registry owns the local
-- PostgreSQL role ID, and exports use the current registered login name.

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

        IF login_id IS NULL THEN
            RAISE EXCEPTION 'A separate unprivileged PostgreSQL login is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF NOT EXISTS (SELECT FROM pg_timezone_names WHERE name = organization_time_zone) THEN
            RAISE EXCEPTION 'Unknown organization time zone' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF NOT (coalesce(allow_initial_import, false) OR coalesce(allow_schedule_updates, false)) THEN
            RAISE EXCEPTION 'Select a desktop capability' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        INSERT INTO vault.organizations (id, name, time_zone)
        VALUES (tenant, organization_name, organization_time_zone);
        INSERT INTO vault.users (id, issuer, subject, display_name)
        VALUES (actor, 'postgresql', actor::text, display_name);
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

CREATE OR REPLACE FUNCTION vault.manager_enroll_member(
    tenant uuid, display_name text, desktop_login name, allow_schedule boolean, allow_time boolean)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        login_id oid;
        member_id uuid;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF nullif(btrim(display_name), '') IS NULL
           OR NOT (coalesce(allow_schedule, false) OR coalesce(allow_time, false)) THEN
            RAISE EXCEPTION 'A member name and desktop capability are required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        SELECT oid INTO login_id FROM pg_roles
        WHERE rolname = desktop_login AND rolcanlogin AND NOT rolsuper
          AND NOT rolbypassrls AND NOT rolcreaterole AND NOT rolcreatedb
          AND NOT rolreplication AND NOT pg_has_role(oid, current_user, 'MEMBER');
        IF login_id IS NULL THEN
            RAISE EXCEPTION 'A separate unprivileged PostgreSQL login is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        PERFORM pg_advisory_xact_lock(173195, (login_id::text::bigint % 2147483647)::integer);
        SELECT principal.user_id INTO member_id
        FROM vault.manager_principals AS principal
        WHERE principal.database_role_id = login_id AND principal.database_role_name = desktop_login
          AND principal.active;
        IF member_id IS NULL THEN
            member_id := gen_random_uuid();
            INSERT INTO vault.users (id, issuer, subject, display_name)
            VALUES (member_id, 'postgresql', member_id::text, display_name);
            INSERT INTO vault.manager_principals (database_role_id, database_role_name, user_id)
            VALUES (login_id, desktop_login, member_id);
        END IF;

        INSERT INTO vault.organization_members (organization_id, user_id, role)
        VALUES (tenant, member_id, 'member')
        ON CONFLICT DO NOTHING;
        IF FOUND THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, vault.context_id('app.current_user_id'), 'organization.member_enrolled',
                    jsonb_build_object('user_id', member_id));
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

CREATE OR REPLACE FUNCTION vault.manager_export_security(tenant uuid) RETURNS jsonb
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        result jsonb;
        members jsonb := '[]'::jsonb;
        actor uuid;
        member_row record;
        identity_row record;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        IF NOT EXISTS (
            SELECT FROM vault.organization_members AS member
            WHERE member.organization_id = tenant
              AND member.user_id = vault.context_id('app.current_user_id')
              AND member.role = 'owner' AND member.active
        ) THEN
            RAISE EXCEPTION 'Organization owner required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        FOR member_row IN
            SELECT member.user_id, member.role, member.active
            FROM vault.organization_members AS member
            WHERE member.organization_id = tenant
            ORDER BY member.user_id
        LOOP
            -- Resolve only identities that belong to the selected organization.
            PERFORM set_config('app.current_user_id', member_row.user_id::text, true);
            SELECT identity.issuer,
                   CASE WHEN identity.issuer = 'postgresql' THEN login.rolname::text
                        ELSE identity.subject END AS subject
            INTO identity_row
            FROM vault.users AS identity
            LEFT JOIN LATERAL (
                SELECT role.rolname
                FROM vault.manager_principals AS principal
                JOIN pg_roles AS role
                  ON (role.oid, role.rolname) = (principal.database_role_id, principal.database_role_name)
                WHERE identity.issuer = 'postgresql' AND principal.user_id = identity.id
                  AND principal.active AND role.rolcanlogin
                ORDER BY (principal.database_role_id::text = identity.subject) DESC, role.rolname
                LIMIT 1
            ) AS login ON true
            WHERE identity.id = member_row.user_id;
            members := members || jsonb_build_array(jsonb_build_object(
                'user_id', member_row.user_id, 'issuer', identity_row.issuer, 'subject', identity_row.subject,
                'role', member_row.role, 'active', member_row.active AND identity_row.issuer IS NOT NULL
            ));
        END LOOP;
        PERFORM set_config('app.current_user_id', actor::text, true);

        SELECT jsonb_build_object(
            'format', 1,
            'organization_id', tenant,
            'members', members,
            'groups', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'group_id', team.id, 'name', team.name, 'members', coalesce((
                        SELECT jsonb_agg(member.user_id ORDER BY member.user_id)
                        FROM vault.group_members AS member
                        WHERE (member.organization_id, member.group_id) = (tenant, team.id)
                    ), '[]'::jsonb)
                ) ORDER BY team.name, team.id)
                FROM vault.groups AS team WHERE team.organization_id = tenant
            ), '[]'::jsonb),
            'projects', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'project_id', project.id, 'title', project.title,
                    'user_grants', coalesce((
                        SELECT jsonb_agg(jsonb_build_object('user_id', grant_row.user_id, 'role', grant_row.role)
                                         ORDER BY grant_row.user_id, grant_row.role)
                        FROM vault.project_members AS grant_row
                        WHERE (grant_row.organization_id, grant_row.project_id) = (tenant, project.id)
                    ), '[]'::jsonb),
                    'group_grants', coalesce((
                        SELECT jsonb_agg(jsonb_build_object('group_id', grant_row.group_id, 'role', grant_row.role)
                                         ORDER BY grant_row.group_id, grant_row.role)
                        FROM vault.project_group_shares AS grant_row
                        WHERE (grant_row.organization_id, grant_row.project_id) = (tenant, project.id)
                    ), '[]'::jsonb)
                ) ORDER BY project.title, project.id)
                FROM vault.projects AS project WHERE project.organization_id = tenant
            ), '[]'::jsonb)
        ) INTO result;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN result;
    END
    $$;
