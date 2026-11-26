-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE vault.time_entries ADD COLUMN task_name_at_entry text;

CREATE FUNCTION vault.capture_time_task_name() RETURNS trigger
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        IF TG_OP = 'INSERT' THEN
            SELECT nullif(task.properties->>'name', '') INTO NEW.task_name_at_entry
            FROM vault.tasks AS task
            WHERE (task.organization_id, task.project_id, task.id)
                = (NEW.organization_id, NEW.project_id, NEW.task_id) AND NOT task.retired;
        ELSIF NEW.task_name_at_entry IS DISTINCT FROM OLD.task_name_at_entry THEN
            RAISE EXCEPTION 'Time entry task name cannot change' USING ERRCODE = 'check_violation';
        END IF;
        RETURN NEW;
    END
    $$;

CREATE TRIGGER capture_time_task_name
    BEFORE INSERT OR UPDATE ON vault.time_entries
    FOR EACH ROW EXECUTE FUNCTION vault.capture_time_task_name();

CREATE FUNCTION vault.desktop_list_time_actions(tenant uuid, project uuid)
    RETURNS TABLE (id uuid, work_date date, work_millis bigint, status text,
                   comments text, billable boolean, can_edit boolean, can_review boolean,
                   submitter_name text, task_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        can_update boolean;
        submitter record;
        identity_name text;
        submitter_names jsonb := '{}'::jsonb;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.desktop_list_time(uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Time entry listing permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        can_update := has_function_privilege(session_user,
            'vault.desktop_update_time(uuid,uuid,uuid,text,text,date,bigint,text,boolean)', 'EXECUTE');

        -- Collect submitters only from entries visible to the reviewing actor.
        FOR submitter IN
            SELECT DISTINCT entry.user_id, member.display_name
            FROM vault.time_entries AS entry
            LEFT JOIN vault.organization_members AS member
              ON (member.organization_id, member.user_id) = (entry.organization_id, entry.user_id)
            WHERE entry.organization_id = tenant AND entry.project_id = project
        LOOP
            PERFORM set_config('app.current_user_id', submitter.user_id::text, true);
            SELECT identity.display_name INTO identity_name
            FROM vault.users AS identity WHERE identity.id = submitter.user_id;
            PERFORM set_config('app.current_user_id', actor::text, true);
            submitter_names := submitter_names || jsonb_build_object(submitter.user_id::text,
                coalesce(nullif(submitter.display_name, ''), nullif(identity_name, ''), submitter.user_id::text));
        END LOOP;

        RETURN QUERY SELECT entry.id, entry.work_date, entry.work_millis, entry.status, entry.comments, entry.billable,
                            can_update AND entry.user_id = actor AND entry.status IN ('draft', 'returned'),
                            can_update AND entry.user_id <> actor AND entry.status = 'submitted',
                            coalesce(submitter_names->>entry.user_id::text, entry.user_id::text),
                            coalesce(entry.task_name_at_entry, nullif(task.properties->>'name', ''))
        FROM vault.time_entries AS entry
        LEFT JOIN vault.tasks AS task
          ON (task.organization_id, task.project_id, task.id)
           = (entry.organization_id, entry.project_id, entry.task_id) AND NOT task.retired
        WHERE entry.organization_id = tenant AND entry.project_id = project
        ORDER BY entry.work_date DESC, entry.id;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
