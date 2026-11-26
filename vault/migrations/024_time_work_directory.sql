-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.desktop_list_time_work(tenant uuid, project uuid)
    RETURNS TABLE (task_id uuid, task_name text, source_uid integer, assignment_id uuid, resource_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        RETURN QUERY
            WITH live_project AS (
                SELECT p.id FROM vault.projects AS p
                WHERE p.organization_id = tenant AND p.id = project AND NOT p.archived
            ), eligible_assignments AS (
                SELECT a.id, a.task_id, a.resource_id
                FROM live_project CROSS JOIN vault.assignments AS a
                WHERE a.organization_id = tenant AND a.project_id = project AND NOT a.retired
                  AND (
                      vault.has_project_role(tenant, project, ARRAY['time_entry'])
                      OR EXISTS (
                          SELECT FROM vault.resource_user_mappings AS mapping
                          WHERE mapping.organization_id = tenant AND mapping.project_id = project
                            AND mapping.resource_id = a.resource_id
                            AND mapping.user_id = vault.context_id('app.current_user_id')
                            AND mapping.effective_until IS NULL
                            AND mapping.effective_from <= transaction_timestamp()
                      )
                  )
            ), eligible_tasks AS (
                SELECT task.id, task.properties->>'name' AS name, task.source_uid, task.position
                FROM live_project CROSS JOIN vault.tasks AS task
                WHERE task.organization_id = tenant AND task.project_id = project AND NOT task.retired
                  AND (vault.has_project_role(tenant, project, ARRAY['time_entry'])
                       OR EXISTS (SELECT FROM eligible_assignments AS a WHERE a.task_id = task.id))
            )
            SELECT work.task_key, work.name, work.uid, work.assignment_key, work.resource
            FROM (
                SELECT task.id AS task_key, task.name, task.source_uid AS uid,
                       NULL::uuid AS assignment_key, NULL::text AS resource, task.position
                FROM eligible_tasks AS task
                UNION ALL
                SELECT task.id, task.name, task.source_uid, assignment.id, resource.properties->>'name',
                       task.position
                FROM eligible_tasks AS task
                JOIN eligible_assignments AS assignment ON assignment.task_id = task.id
                LEFT JOIN vault.resources AS resource
                  ON resource.organization_id = tenant AND resource.project_id = project
                 AND resource.id = assignment.resource_id AND NOT resource.retired
            ) AS work
            ORDER BY work.position, work.task_key, work.assignment_key NULLS FIRST;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.desktop_list_time_work(uuid, uuid) FROM PUBLIC;
