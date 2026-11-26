-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

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
