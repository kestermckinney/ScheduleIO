-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE POLICY read_own_mapped_live_resources ON vault.resources FOR SELECT
    USING (
        NOT retired AND EXISTS (
            SELECT FROM vault.resource_user_mappings AS mapping
            WHERE (mapping.organization_id, mapping.project_id, mapping.resource_id)
                = (resources.organization_id, resources.project_id, resources.id)
              AND mapping.user_id = vault.context_id('app.current_user_id')
              AND mapping.effective_from <= transaction_timestamp()
              AND mapping.effective_until IS NULL
        )
    );

CREATE FUNCTION vault.desktop_list_schedule_assignments(tenant uuid, project uuid)
    RETURNS TABLE (task_id uuid, assignment_id uuid, resource_name text, work_millis bigint)
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
            SELECT task.id, assignment.id, resource.properties->>'name',
                   (assignment.properties->>'work_millis')::bigint
            FROM vault.tasks AS task
            JOIN vault.assignments AS assignment
              ON (assignment.organization_id, assignment.project_id, assignment.task_id)
               = (task.organization_id, task.project_id, task.id) AND NOT assignment.retired
            LEFT JOIN vault.resources AS resource
              ON (resource.organization_id, resource.project_id, resource.id)
               = (assignment.organization_id, assignment.project_id, assignment.resource_id)
             AND NOT resource.retired
            WHERE task.organization_id = tenant AND task.project_id = project AND NOT task.retired
            ORDER BY task.position, assignment.position, assignment.id;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
