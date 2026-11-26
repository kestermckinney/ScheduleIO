-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.desktop_list_schedule_dependencies(tenant uuid, project uuid)
    RETURNS TABLE (task_id uuid, predecessor_id uuid, predecessor_name text, predecessor_uid integer,
                   link_type integer, lag_millis bigint)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.desktop_list_projects(uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Project directory permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        RETURN QUERY
            SELECT task.id, predecessor.id, predecessor.properties->>'name', predecessor.source_uid,
                   link.link_type, link.lag_millis
            FROM vault.tasks AS task
            JOIN vault.projects AS schedule
              ON (schedule.organization_id, schedule.id) = (task.organization_id, task.project_id)
            CROSS JOIN LATERAL jsonb_to_recordset(task.properties->'predecessors')
                AS link(predecessor_uid integer, link_type integer, lag_millis bigint)
            JOIN vault.tasks AS predecessor
              ON (predecessor.organization_id, predecessor.project_id, predecessor.source_uid)
               = (task.organization_id, task.project_id, link.predecessor_uid)
             AND NOT predecessor.retired
            WHERE task.organization_id = tenant AND task.project_id = project AND NOT task.retired
            ORDER BY task.position, predecessor.position, predecessor.id, link.link_type, link.lag_millis;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
