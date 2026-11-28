-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- A schema owner can grant named sets of entry points to a registered login.
-- Vault membership and project roles remain checked by those entry points.
CREATE FUNCTION vault.grant_desktop_capability_privileges(desktop_login name, capability text)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        routines text[];
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
            WHERE login.rolname = desktop_login AND login.rolcanlogin AND principal.active
              AND NOT login.rolsuper AND NOT login.rolbypassrls AND NOT login.rolcreaterole
              AND NOT login.rolcreatedb AND NOT login.rolreplication
              AND NOT pg_has_role(login.oid, current_user, 'MEMBER')
        ) THEN
            RAISE EXCEPTION 'Select an active, registered, unprivileged Vault login'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        CASE capability
            WHEN 'schedule_updates' THEN
                routines := ARRAY[
                    'vault.desktop_read_schedule(uuid,uuid)',
                    'vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb)',
                    'vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)',
                    'vault.manager_lookup_import_source(uuid,text,text,text)'];
            WHEN 'directory' THEN
                routines := ARRAY['vault.desktop_list_projects(uuid)'];
            WHEN 'management' THEN
                routines := ARRAY[
                    'vault.manager_enroll_member(uuid,text,name,boolean,boolean)',
                    'vault.manager_set_member_active(uuid,uuid,boolean)',
                    'vault.manager_change_group(uuid,uuid,text,text,uuid)',
                    'vault.manager_change_project_share(uuid,uuid,text,uuid,text,boolean)',
                    'vault.manager_set_project_archived(uuid,uuid,boolean)',
                    'vault.manager_create_login(uuid,name,text)',
                    'vault.manager_enable_member_capability(uuid,uuid,text)',
                    'vault.manager_list_member_access(uuid)',
                    'vault.manager_set_member_access(uuid,uuid,boolean,boolean)',
                    'vault.manager_sign_in(uuid)',
                    'vault.manager_set_sign_in(uuid,text,text,text)',
                    'vault.manager_set_sign_in_email(uuid,uuid,text)',
                    'vault.manager_set_member_password(uuid,uuid,text)'];
            WHEN 'resources' THEN
                routines := ARRAY[
                    'vault.manager_list_resources(uuid,uuid)',
                    'vault.manager_set_resource_user(uuid,uuid,uuid,uuid)',
                    'vault.desktop_portfolio_work(uuid,uuid)'];
            WHEN 'audit' THEN
                routines := ARRAY[
                    'vault.manager_list_audit(uuid,uuid,integer)',
                    'vault.manager_check_project_access(uuid,uuid,uuid)'];
            WHEN 'transfers' THEN
                routines := ARRAY[
                    'vault.manager_record_transfer(uuid,uuid,uuid,text,boolean,text)',
                    'vault.manager_list_transfers(uuid)'];
            WHEN 'export' THEN
                routines := ARRAY[
                    'vault.desktop_read_schedule(uuid,uuid)',
                    'vault.desktop_approved_export_work(uuid,uuid)'];
            WHEN 'time' THEN
                routines := ARRAY[
                    'vault.desktop_list_time_work(uuid,uuid)',
                    'vault.desktop_list_time(uuid,uuid)',
                    'vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean)',
                    'vault.desktop_update_time(uuid,uuid,uuid,text,text,date,bigint,text,boolean)'];
            ELSE
                RAISE EXCEPTION 'Unknown Vault desktop capability' USING ERRCODE = 'invalid_parameter_value';
        END CASE;

        EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', desktop_login);
        FOREACH routine IN ARRAY routines LOOP
            EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO %I', routine, desktop_login);
        END LOOP;
    END
    $$;

REVOKE ALL ON FUNCTION vault.grant_desktop_capability_privileges(name, text) FROM PUBLIC;

CREATE FUNCTION vault.revoke_desktop_schedule_update_privileges(desktop_login text)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        login_id oid;
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
        SELECT login.oid INTO login_id
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login
          ON (login.oid, login.rolname) = (principal.database_role_id, principal.database_role_name)
        WHERE login.rolname::text = desktop_login;
        IF login_id IS NULL THEN
            RAISE EXCEPTION 'Select a registered Vault login' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        EXECUTE format(
            'REVOKE EXECUTE ON FUNCTION
                vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb),
                vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)
             FROM %I', desktop_login);

        -- Reject an apparently successful revocation if another database role
        -- still grants the same capability. The exception rolls back the change.
        IF EXISTS (
            SELECT FROM pg_roles AS candidate WHERE pg_has_role(login_id, candidate.oid, 'MEMBER')
              AND (has_function_privilege(candidate.oid,
                    'vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb)', 'EXECUTE')
                   OR has_function_privilege(candidate.oid,
                    'vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)', 'EXECUTE'))
        ) THEN
            RAISE EXCEPTION 'Another PostgreSQL role still provides schedule updates to this login'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
    END
    $$;

REVOKE ALL ON FUNCTION vault.revoke_desktop_schedule_update_privileges(text) FROM PUBLIC;

CREATE FUNCTION vault.enable_desktop_capability(desktop_login name, capability text)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        PERFORM vault.grant_desktop_capability_privileges(desktop_login, capability);
        INSERT INTO vault_admin.desktop_capability_events
            (operator_login, operator_role, target_login, capability, action)
        VALUES (session_user, current_user, desktop_login, capability, 'enabled');
    END
    $$;

REVOKE ALL ON FUNCTION vault.enable_desktop_capability(name, text) FROM PUBLIC;

CREATE FUNCTION vault.revoke_desktop_schedule_updates(desktop_login text)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        PERFORM vault.revoke_desktop_schedule_update_privileges(desktop_login);
        INSERT INTO vault_admin.desktop_capability_events
            (operator_login, operator_role, target_login, capability, action)
        VALUES (session_user, current_user, desktop_login::name, 'schedule_updates', 'revoked');
    END
    $$;

REVOKE ALL ON FUNCTION vault.revoke_desktop_schedule_updates(text) FROM PUBLIC;

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

        INSERT INTO vault_admin.desktop_capability_events
            (operator_login, operator_role, target_login, capability, action)
        SELECT session_user, current_user, item.database_role_name, 'desktop_login', 'rebound'
        FROM jsonb_to_recordset(bindings) AS item(database_role_name name, requires_change boolean)
        WHERE item.requires_change;
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.rebind_restored_desktop_logins(text[]) FROM PUBLIC;

REVOKE ALL ON vault_admin.desktop_capability_events FROM PUBLIC;
