-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- What a Vault login may call comes from the person's organization role:
--   every member gets the ScheduleVault entry points (projects, schedules,
--   export, transfers, and time), and owners and administrators also get the
--   Manager entry points. Each entry point still checks the caller's
--   organization role and project access, which decide what is allowed.

-- The entry points a login gets: the member set, or the Manager set.
CREATE FUNCTION vault.login_routines(manager boolean) RETURNS text[]
    LANGUAGE sql IMMUTABLE
    SET search_path = pg_catalog
    AS $$
        SELECT CASE WHEN manager THEN ARRAY[
            'vault.manager_enroll_member(uuid,text,name)',
            'vault.manager_set_member_active(uuid,uuid,boolean)',
            'vault.manager_change_group(uuid,uuid,text,text,uuid)',
            'vault.manager_change_project_share(uuid,uuid,text,uuid,text,boolean)',
            'vault.manager_set_project_archived(uuid,uuid,boolean)',
            'vault.manager_create_login(uuid,name,text)',
            'vault.manager_sign_in(uuid)',
            'vault.manager_set_sign_in(uuid,text,text,text)',
            'vault.manager_set_sign_in_email(uuid,uuid,text)',
            'vault.manager_set_member_password(uuid,uuid,text)',
            'vault.manager_list_resources(uuid,uuid)',
            'vault.manager_set_resource_user(uuid,uuid,uuid,uuid)',
            'vault.desktop_portfolio_work(uuid,uuid)',
            'vault.manager_list_audit(uuid,uuid,integer)',
            'vault.manager_check_project_access(uuid,uuid,uuid)']
        ELSE ARRAY[
            'vault.desktop_list_projects(uuid)',
            'vault.desktop_read_schedule(uuid,uuid)',
            'vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb)',
            'vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)',
            'vault.manager_lookup_import_source(uuid,text,text,text)',
            'vault.desktop_approved_export_work(uuid,uuid)',
            'vault.manager_record_transfer(uuid,uuid,uuid,text,boolean,text)',
            'vault.manager_list_transfers(uuid)',
            'vault.desktop_list_time_work(uuid,uuid)',
            'vault.desktop_list_time(uuid,uuid)',
            'vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean)',
            'vault.desktop_update_time(uuid,uuid,uuid,text,text,date,bigint,text,boolean)']
        END
    $$;

REVOKE ALL ON FUNCTION vault.login_routines(boolean) FROM PUBLIC;

-- The login registered to a Vault user, preferring an active one.
CREATE FUNCTION vault.member_login(member uuid) RETURNS name
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT login.rolname
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login
          ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
        WHERE principal.user_id = member AND login.rolcanlogin
        ORDER BY NOT principal.active, login.rolname LIMIT 1
    $$;

REVOKE ALL ON FUNCTION vault.member_login(uuid) FROM PUBLIC;

-- Internal: grants (or, for the Manager set, also revokes) a registered,
-- unprivileged login's entry points. Runs as the Vault schema owner.
CREATE FUNCTION vault.set_login_routines(desktop_login name, manager boolean, enabled boolean)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        routine text;
    BEGIN
        IF NOT EXISTS (
            SELECT FROM pg_namespace AS schema
            JOIN pg_roles AS owner ON owner.oid = schema.nspowner
            WHERE schema.nspname = 'vault' AND owner.rolname = current_user
              AND NOT owner.rolsuper AND NOT owner.rolbypassrls
        ) THEN
            RAISE EXCEPTION 'Connect as the Vault schema owner'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF NOT EXISTS (
            SELECT FROM vault.manager_principals AS principal
            JOIN pg_roles AS login
              ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
            WHERE login.rolname = desktop_login AND login.rolcanlogin
              AND NOT login.rolsuper AND NOT login.rolbypassrls AND NOT login.rolcreaterole
              AND NOT login.rolcreatedb AND NOT login.rolreplication
              AND NOT pg_has_role(login.oid, current_user, 'MEMBER')
        ) THEN
            RAISE EXCEPTION 'Select a registered, unprivileged Vault login'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF NOT (enabled OR manager) THEN
            RAISE EXCEPTION 'Member entry points are not revoked; deactivate the member instead'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        IF enabled THEN
            EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', desktop_login);
        END IF;
        FOREACH routine IN ARRAY vault.login_routines(manager) LOOP
            IF enabled THEN
                EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO %I', routine, desktop_login);
            ELSE
                EXECUTE format('REVOKE EXECUTE ON FUNCTION %s FROM %I', routine, desktop_login);
            END IF;
        END LOOP;
    END
    $$;

REVOKE ALL ON FUNCTION vault.set_login_routines(name, boolean, boolean) FROM PUBLIC;

-- Internal: gives a member's login the Manager entry points when they are an
-- owner or administrator, and takes them away when they become a member.
CREATE FUNCTION vault.sync_manager_routines(member uuid, manager boolean)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        member_login name := vault.member_login(member);
    BEGIN
        IF member_login IS NOT NULL THEN
            PERFORM vault.set_login_routines(member_login, true, manager);
        END IF;
    END
    $$;

REVOKE ALL ON FUNCTION vault.sync_manager_routines(uuid, boolean) FROM PUBLIC;

CREATE FUNCTION vault.manager_check_desktop_project_access(tenant uuid, project uuid, target_user uuid)
    RETURNS TABLE (login_name text, project_visible boolean, schedule_visible boolean, can_save boolean,
                   can_reimport boolean, can_review_time boolean)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_check_project_access(uuid,uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Project access diagnostics permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);

        RETURN QUERY
        WITH project_access AS MATERIALIZED (
            SELECT * FROM vault.manager_check_project_access(tenant, project, target_user)
        ), active_logins AS (
            SELECT login.oid, login.rolname
            FROM vault.manager_principals AS principal
            JOIN pg_roles AS login
              ON (login.oid, login.rolname) = (principal.database_role_id, principal.database_role_name)
            JOIN vault.organization_members AS member
              ON member.organization_id = tenant AND member.user_id = principal.user_id AND member.active
            WHERE principal.user_id = target_user AND principal.active AND login.rolcanlogin
              AND has_database_privilege(login.oid, current_database(), 'CONNECT')
              AND has_schema_privilege(login.oid, 'vault', 'USAGE')
        )
        SELECT login.rolname::text,
               coalesce(access.project_visible AND has_function_privilege(login.oid,
                   'vault.desktop_list_projects(uuid)', 'EXECUTE'), false),
               coalesce(access.full_schedule_visible AND has_function_privilege(login.oid,
                   'vault.desktop_read_schedule(uuid,uuid)', 'EXECUTE'), false),
               coalesce(access.schedule_writable AND access.full_schedule_visible
                   AND has_function_privilege(login.oid, 'vault.desktop_read_schedule(uuid,uuid)', 'EXECUTE')
                   AND has_function_privilege(login.oid,
                       'vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb)', 'EXECUTE'), false),
               coalesce(access.schedule_writable AND access.full_schedule_visible
                   AND has_function_privilege(login.oid, 'vault.desktop_read_schedule(uuid,uuid)', 'EXECUTE')
                   AND has_function_privilege(login.oid, 'vault.manager_lookup_import_source(uuid,text,text,text)', 'EXECUTE')
                   AND has_function_privilege(login.oid,
                       'vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)', 'EXECUTE'), false),
               coalesce(access.can_review_time
                   AND has_function_privilege(login.oid, 'vault.desktop_list_time(uuid,uuid)', 'EXECUTE')
                   AND has_function_privilege(login.oid,
                       'vault.desktop_update_time(uuid,uuid,uuid,text,text,date,bigint,text,boolean)', 'EXECUTE'), false)
        FROM project_access AS access LEFT JOIN active_logins AS login ON true
        ORDER BY login.rolname;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

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

        INSERT INTO vault_admin.platform_events (operator_login, operation, details)
        SELECT session_user, 'vault.login_rebound', jsonb_build_object('login', item.database_role_name)
        FROM jsonb_to_recordset(bindings) AS item(database_role_name name, requires_change boolean)
        WHERE item.requires_change;
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.rebind_restored_desktop_logins(text[]) FROM PUBLIC;
