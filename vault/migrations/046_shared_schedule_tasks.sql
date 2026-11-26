-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- Return only the fields used by the shared task view. The task policy applies
-- to both the listed task and its parent, including for assignment-only users.
CREATE FUNCTION vault.desktop_list_schedule_tasks(tenant uuid, project uuid)
    RETURNS TABLE (task_id uuid, parent_id uuid, task_name text, source_uid integer,
                   starts_at timestamptz, finishes_at timestamptz, percent_complete double precision,
                   milestone boolean, summary boolean)
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
            SELECT task.id, parent.id, task.properties->>'name', task.source_uid,
                   nullif(task.properties->>'start', '')::timestamptz,
                   nullif(task.properties->>'finish', '')::timestamptz,
                   (task.properties->>'percent_complete')::double precision,
                   (task.properties->>'milestone')::boolean, (task.properties->>'summary')::boolean
            FROM vault.tasks AS task
            JOIN vault.projects AS schedule
              ON (schedule.organization_id, schedule.id) = (task.organization_id, task.project_id)
            LEFT JOIN vault.tasks AS parent
              ON (parent.organization_id, parent.project_id, parent.id)
               = (task.organization_id, task.project_id, task.parent_id) AND NOT parent.retired
            WHERE task.organization_id = tenant AND task.project_id = project AND NOT task.retired
            ORDER BY task.position, task.id;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
