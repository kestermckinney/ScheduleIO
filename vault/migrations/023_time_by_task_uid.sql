-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.desktop_create_task_time(
    tenant uuid, project uuid, entry_id uuid, task_uid integer,
    day date, millis bigint, notes text, is_billable boolean)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        task_id_value uuid;
        created_id uuid;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        SELECT task.id INTO task_id_value FROM vault.tasks AS task
        WHERE task.organization_id = tenant AND task.project_id = project
          AND task.source_uid = task_uid AND NOT task.retired;
        IF task_id_value IS NULL THEN
            RAISE EXCEPTION 'Task is unavailable for time entry' USING ERRCODE = 'insufficient_privilege';
        END IF;

        created_id := vault.desktop_create_time(
            tenant, project, entry_id, task_id_value, NULL, day, millis, notes, is_billable);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN created_id;
    END
    $$;

REVOKE ALL ON FUNCTION vault.desktop_create_task_time(uuid, uuid, uuid, integer, date, bigint, text, boolean)
    FROM PUBLIC;
