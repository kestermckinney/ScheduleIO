-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.desktop_list_time(tenant uuid, project uuid)
    RETURNS SETOF vault.time_entries
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        RETURN QUERY SELECT entry.* FROM vault.time_entries AS entry
        WHERE entry.organization_id = tenant AND entry.project_id = project
        ORDER BY entry.work_date DESC, entry.id;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

CREATE FUNCTION vault.desktop_create_time(
    tenant uuid, project uuid, entry_id uuid, task uuid, assignment uuid,
    day date, millis bigint, notes text, is_billable boolean)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        INSERT INTO vault.time_entries
            (organization_id, project_id, id, user_id, task_id, assignment_id,
             work_date, work_millis, comments, billable)
        VALUES (tenant, project, entry_id, vault.context_id('app.current_user_id'),
                task, assignment, day, millis, notes, is_billable);

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN entry_id;
    END
    $$;

CREATE FUNCTION vault.desktop_update_time(
    tenant uuid, project uuid, entry_id uuid, expected_status text, next_status text,
    day date, millis bigint, notes text, is_billable boolean)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        changed boolean;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        UPDATE vault.time_entries AS entry
        SET status = next_status,
            work_date = coalesce(day, entry.work_date),
            work_millis = coalesce(millis, entry.work_millis),
            comments = coalesce(notes, entry.comments),
            billable = coalesce(is_billable, entry.billable)
        WHERE entry.organization_id = tenant AND entry.project_id = project
          AND entry.id = entry_id AND entry.status = expected_status;
        changed := FOUND;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.desktop_list_time(uuid, uuid) FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.desktop_create_time(uuid, uuid, uuid, uuid, uuid, date, bigint, text, boolean)
    FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.desktop_update_time(uuid, uuid, uuid, text, text, date, bigint, text, boolean)
    FROM PUBLIC;
