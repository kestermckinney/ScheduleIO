-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- Desktop connections use individual logins registered by the Vault operator.
-- Grant only these entry points to ScheduleVault logins, not direct table
-- access or the context-taking operations reserved for trusted services.
CREATE FUNCTION vault.desktop_read_schedule(tenant uuid, project uuid)
    RETURNS TABLE (
        record_kind text, row_position integer, source_uid integer, record_id uuid,
        edit_counter bigint, format_revision integer, model_data bytea)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        RETURN QUERY
            WITH accessible_schedule AS (
                SELECT data.project_metadata, data.project_collections,
                       data.format_revision, p.edit_counter
                FROM vault.schedule_data AS data
                JOIN vault.projects AS p ON (p.organization_id, p.id) = (data.organization_id, data.project_id)
                WHERE data.organization_id = tenant AND data.project_id = project
                  AND vault.has_project_role(tenant, project, ARRAY['manager'])
            )
            SELECT 'metadata'::text, 0, NULL::integer, NULL::uuid, data.edit_counter,
                   data.format_revision, data.project_metadata
            FROM accessible_schedule AS data
            UNION ALL
            SELECT 'collections', 0, NULL, NULL, NULL, NULL, data.project_collections
            FROM accessible_schedule AS data
            UNION ALL
            SELECT 'task', task.position, task.source_uid, task.id, NULL, NULL, task.model_data
            FROM accessible_schedule CROSS JOIN vault.tasks AS task
            WHERE task.organization_id = tenant AND task.project_id = project AND NOT task.retired
            UNION ALL
            SELECT 'resource', resource.position, resource.source_uid, resource.id, NULL, NULL, resource.model_data
            FROM accessible_schedule CROSS JOIN vault.resources AS resource
            WHERE resource.organization_id = tenant AND resource.project_id = project AND NOT resource.retired
            UNION ALL
            SELECT 'assignment', assignment.position, assignment.source_uid, assignment.id, NULL, NULL, assignment.model_data
            FROM accessible_schedule CROSS JOIN vault.assignments AS assignment
            WHERE assignment.organization_id = tenant AND assignment.project_id = project AND NOT assignment.retired;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

CREATE FUNCTION vault.desktop_save_schedule(
    tenant uuid, project uuid, expected_counter bigint, title text,
    metadata bytea, collections bytea, task_rows jsonb, resource_rows jsonb, assignment_rows jsonb)
    RETURNS bigint
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        saved_counter bigint;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_write_schedule(tenant, project), false) THEN
            RAISE EXCEPTION 'Schedule unavailable for editing' USING ERRCODE = 'insufficient_privilege';
        END IF;
        saved_counter := vault.save_schedule(
            tenant, project, expected_counter, title,
            metadata, collections, task_rows, resource_rows, assignment_rows);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN saved_counter;
    END
    $$;

REVOKE ALL ON FUNCTION vault.desktop_read_schedule(uuid, uuid) FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.desktop_save_schedule(uuid, uuid, bigint, text, bytea, bytea, jsonb, jsonb, jsonb)
    FROM PUBLIC;
