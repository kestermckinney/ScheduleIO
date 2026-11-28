-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- ── Time ──
-- A person sees their own time; Edit sees everyone's on the project.
CREATE FUNCTION vault.can_read_time_entry(tenant uuid, project uuid, logger uuid)
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT tenant = vault.context_id('app.current_organization_id')
           AND EXISTS (
               SELECT FROM vault.organization_members AS member
               WHERE member.organization_id = tenant
                 AND member.user_id = vault.context_id('app.current_user_id')
           )
           AND (logger = vault.context_id('app.current_user_id')
                OR vault.has_project_role(tenant, project, ARRAY['editor']))
    $$;

-- People change and submit their own drafts; Edit can do the same for anyone
-- on the project, and approves or returns other people's submitted time.
CREATE FUNCTION vault.guard_time_entry_update() RETURNS trigger
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        actor uuid := vault.context_id('app.current_user_id');
        can_edit boolean := coalesce(vault.has_project_role(OLD.organization_id, OLD.project_id, ARRAY['editor']), false);
    BEGIN
        IF ROW(NEW.organization_id, NEW.project_id, NEW.id, NEW.user_id,
               NEW.task_id, NEW.assignment_id)
            IS DISTINCT FROM
           ROW(OLD.organization_id, OLD.project_id, OLD.id, OLD.user_id,
               OLD.task_id, OLD.assignment_id) THEN
            RAISE EXCEPTION 'Time entry identity cannot change' USING ERRCODE = 'check_violation';
        END IF;

        IF OLD.status IN ('draft', 'returned') AND (actor = OLD.user_id OR can_edit)
            AND NEW.status IN ('draft', 'submitted') THEN
            RETURN NEW;
        END IF;

        IF OLD.status = 'submitted' AND actor <> OLD.user_id AND can_edit
            AND NEW.status IN ('approved', 'returned')
            AND ROW(NEW.work_date, NEW.work_millis, NEW.comments, NEW.billable)
                IS NOT DISTINCT FROM
                ROW(OLD.work_date, OLD.work_millis, OLD.comments, OLD.billable) THEN
            RETURN NEW;
        END IF;

        RAISE EXCEPTION 'Invalid time entry transition' USING ERRCODE = 'check_violation';
    END
    $$;

CREATE FUNCTION vault.record_time_entry_event() RETURNS trigger
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        event text;
    BEGIN
        IF TG_OP = 'INSERT' THEN
            event := 'created';
        ELSIF NEW.status IN ('submitted', 'approved', 'returned')
            AND NEW.status IS DISTINCT FROM OLD.status THEN
            event := NEW.status;
        ELSE
            RETURN NULL;
        END IF;

        INSERT INTO vault.time_entry_events
            (organization_id, project_id, entry_id, actor_id, event_type)
        VALUES (NEW.organization_id, NEW.project_id, NEW.id,
                vault.context_id('app.current_user_id'), event);

        RETURN NULL;
    END
    $$;

REVOKE ALL ON FUNCTION vault.record_time_entry_event() FROM PUBLIC;

CREATE FUNCTION vault.manager_set_resource_user(
    tenant uuid, project uuid, resource uuid, member_id uuid)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        current_mapping vault.resource_user_mappings%ROWTYPE;
        changed_at timestamptz := clock_timestamp();
        eligible boolean;
        actor uuid;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_write_schedule(tenant, project), false) THEN
            RAISE EXCEPTION 'Project unavailable for resource mapping' USING ERRCODE = 'insufficient_privilege';
        END IF;

        IF member_id IS NOT NULL THEN
            IF NOT EXISTS (
                SELECT FROM vault.organization_members AS member
                WHERE member.organization_id = tenant AND member.user_id = member_id AND member.active
            ) THEN
                RAISE EXCEPTION 'Select an active organization user' USING ERRCODE = 'invalid_parameter_value';
            END IF;
            actor := vault.context_id('app.current_user_id');
            PERFORM set_config('app.current_user_id', member_id::text, true);
            SELECT EXISTS (SELECT FROM vault.users AS identity WHERE identity.id = member_id AND identity.active)
            INTO eligible;
            PERFORM set_config('app.current_user_id', actor::text, true);
            IF NOT eligible THEN
                RAISE EXCEPTION 'Select an active organization user' USING ERRCODE = 'invalid_parameter_value';
            END IF;
        END IF;

        SELECT NOT row.retired AND row.properties->>'kind' = 'work'
               AND NOT (row.source_uid IS NOT DISTINCT FROM 0 AND coalesce(btrim(row.properties->>'name'), '') = '')
        INTO eligible
        FROM vault.resources AS row
        WHERE row.organization_id = tenant AND row.project_id = project AND row.id = resource
        FOR UPDATE;
        IF NOT FOUND OR (member_id IS NOT NULL AND eligible IS DISTINCT FROM true) THEN
            RAISE EXCEPTION 'Select a live work resource' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        SELECT * INTO current_mapping
        FROM vault.resource_user_mappings AS mapping
        WHERE mapping.organization_id = tenant AND mapping.project_id = project
          AND mapping.resource_id = resource AND mapping.effective_until IS NULL;

        IF current_mapping.user_id IS NOT DISTINCT FROM member_id THEN
            PERFORM set_config('app.current_organization_id', previous_tenant, true);
            PERFORM set_config('app.current_user_id', previous_actor, true);
            RETURN false;
        END IF;

        IF current_mapping.id IS NOT NULL THEN
            changed_at := greatest(changed_at, current_mapping.effective_from + interval '1 microsecond');
            UPDATE vault.resource_user_mappings SET effective_until = changed_at
            WHERE organization_id = tenant AND project_id = project AND id = current_mapping.id;
        END IF;

        IF member_id IS NOT NULL THEN
            INSERT INTO vault.resource_user_mappings
                (organization_id, project_id, id, resource_id, user_id, effective_from)
            VALUES (tenant, project, gen_random_uuid(), resource, member_id, changed_at);
        END IF;

        INSERT INTO vault.audit_events (organization_id, project_id, actor_id, operation, details)
        VALUES (tenant, project, vault.context_id('app.current_user_id'), 'resource.mapping_changed',
                jsonb_build_object('resource_id', resource, 'previous_user_id', current_mapping.user_id,
                                   'user_id', member_id));

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN true;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_set_resource_user(uuid, uuid, uuid, uuid) FROM PUBLIC;

CREATE FUNCTION vault.manager_list_resources(tenant uuid, project uuid)
    RETURNS TABLE (resource_id uuid, resource_name text, resource_kind text, user_id uuid)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_write_schedule(tenant, project), false) THEN
            RAISE EXCEPTION 'Project unavailable for resource mapping' USING ERRCODE = 'insufficient_privilege';
        END IF;

        RETURN QUERY
        SELECT resource.id, resource.properties->>'name',
               CASE WHEN resource.properties->>'kind' = 'work' AND resource.source_uid = 0
                         AND coalesce(btrim(resource.properties->>'name'), '') = '' THEN 'placeholder'
                    ELSE resource.properties->>'kind' END,
               mapping.user_id
        FROM vault.resources AS resource
        LEFT JOIN vault.resource_user_mappings AS mapping
          ON (mapping.organization_id, mapping.project_id, mapping.resource_id)
             = (resource.organization_id, resource.project_id, resource.id)
         AND mapping.effective_until IS NULL
        WHERE resource.organization_id = tenant AND resource.project_id = project AND NOT resource.retired
        ORDER BY resource.position, resource.id;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_list_resources(uuid, uuid) FROM PUBLIC;

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

REVOKE ALL ON FUNCTION vault.desktop_list_time(uuid, uuid) FROM PUBLIC;

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

REVOKE ALL ON FUNCTION vault.desktop_create_time(uuid, uuid, uuid, uuid, uuid, date, bigint, text, boolean)
    FROM PUBLIC;

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

REVOKE ALL ON FUNCTION vault.desktop_update_time(uuid, uuid, uuid, text, text, date, bigint, text, boolean)
    FROM PUBLIC;

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

CREATE FUNCTION vault.desktop_approved_export_work(tenant uuid, project uuid)
    RETURNS TABLE (
        assignment_uid integer, period_start timestamptz, period_end timestamptz,
        work_millis numeric, entry_count bigint, work_date date)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.has_project_role(tenant, project, ARRAY['manager']), false) THEN
            RAISE EXCEPTION 'Full schedule access required for actual work export'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        RETURN QUERY
        SELECT assignment.source_uid,
               entry.work_date::timestamp AT TIME ZONE organization.time_zone,
               (entry.work_date + 1)::timestamp AT TIME ZONE organization.time_zone,
               sum(entry.work_millis), count(*), entry.work_date
        FROM vault.time_entries AS entry
        JOIN vault.organizations AS organization ON organization.id = entry.organization_id
        LEFT JOIN vault.assignments AS assignment
          ON (assignment.organization_id, assignment.project_id, assignment.id)
             = (entry.organization_id, entry.project_id, entry.assignment_id) AND NOT assignment.retired
        WHERE entry.organization_id = tenant AND entry.project_id = project AND entry.status = 'approved'
        GROUP BY assignment.source_uid, entry.work_date, organization.time_zone
        ORDER BY assignment.source_uid NULLS LAST, entry.work_date;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.desktop_approved_export_work(uuid, uuid) FROM PUBLIC;

CREATE FUNCTION vault.manager_list_mapping_users(tenant uuid, project uuid)
    RETURNS TABLE (user_id uuid, display_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        member record;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_resource_user(uuid,uuid,uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Resource mapping permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_write_schedule(tenant, project), false) THEN
            RAISE EXCEPTION 'Project unavailable for resource mapping' USING ERRCODE = 'insufficient_privilege';
        END IF;

        FOR member IN SELECT membership.user_id, membership.display_name
                      FROM vault.organization_members AS membership
                      WHERE membership.organization_id = tenant AND membership.active
                        AND NOT vault.is_service_account(membership.user_id)
        LOOP
            PERFORM set_config('app.current_user_id', member.user_id::text, true);
            RETURN QUERY SELECT identity.id, coalesce(member.display_name, identity.display_name)
                FROM vault.users AS identity WHERE identity.id = member.user_id AND identity.active;
        END LOOP;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

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
        has_edit boolean;
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
        has_edit := coalesce(vault.has_project_role(tenant, project, ARRAY['editor']), false);

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
                            can_update AND (entry.user_id = actor OR has_edit) AND entry.status IN ('draft', 'returned'),
                            can_update AND has_edit AND entry.user_id <> actor AND entry.status = 'submitted',
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

CREATE TRIGGER guard_time_entry_update
    BEFORE UPDATE ON vault.time_entries
    FOR EACH ROW EXECUTE FUNCTION vault.guard_time_entry_update();

CREATE TRIGGER record_time_entry_event
    AFTER INSERT OR UPDATE ON vault.time_entries
    FOR EACH ROW EXECUTE FUNCTION vault.record_time_entry_event();

CREATE TRIGGER capture_time_task_name
    BEFORE INSERT OR UPDATE ON vault.time_entries
    FOR EACH ROW EXECUTE FUNCTION vault.capture_time_task_name();

CREATE POLICY read_own_active_mappings ON vault.resource_user_mappings FOR SELECT
    USING (
        effective_until IS NULL
        AND user_id = vault.context_id('app.current_user_id')
        AND EXISTS (
            SELECT 1 FROM vault.projects AS project
            WHERE project.organization_id = resource_user_mappings.organization_id
              AND project.id = resource_user_mappings.project_id
        )
    );

CREATE POLICY read_time_entries ON vault.time_entries FOR SELECT
    USING (vault.can_read_time_entry(organization_id, project_id, user_id));

CREATE POLICY read_visible_time_entry_events ON vault.time_entry_events FOR SELECT
    USING (
        EXISTS (
            SELECT 1 FROM vault.time_entries AS entry
            WHERE entry.organization_id = time_entry_events.organization_id
              AND entry.project_id = time_entry_events.project_id
              AND entry.id = time_entry_events.entry_id
        )
    );

CREATE POLICY update_visible_time_entries ON vault.time_entries FOR UPDATE
    USING (
        vault.can_read_time_entry(organization_id, project_id, user_id)
        AND (
            (status IN ('draft', 'returned')
             AND (user_id = vault.context_id('app.current_user_id')
                  OR vault.has_project_role(organization_id, project_id, ARRAY['editor'])))
            OR (user_id <> vault.context_id('app.current_user_id') AND status = 'submitted'
                AND vault.has_project_role(organization_id, project_id, ARRAY['editor']))
        )
    )
    WITH CHECK (
        vault.can_read_time_entry(organization_id, project_id, user_id)
        AND (
            (status IN ('draft', 'submitted')
             AND (user_id = vault.context_id('app.current_user_id')
                  OR vault.has_project_role(organization_id, project_id, ARRAY['editor'])))
            OR (user_id <> vault.context_id('app.current_user_id') AND status IN ('approved', 'returned')
                AND vault.has_project_role(organization_id, project_id, ARRAY['editor']))
        )
    );

CREATE POLICY read_live_assignments ON vault.assignments FOR SELECT
    USING (NOT retired AND vault.has_project_role(organization_id, project_id, ARRAY['viewer']));

CREATE POLICY read_live_tasks ON vault.tasks FOR SELECT
    USING (
        NOT retired AND (
            vault.has_project_role(organization_id, project_id, ARRAY['manager', 'editor', 'viewer', 'time_entry'])
            OR EXISTS (
                SELECT FROM vault.assignments AS assignment
                WHERE assignment.organization_id = tasks.organization_id
                  AND assignment.project_id = tasks.project_id
                  AND assignment.task_id = tasks.id
            )
        )
    );

-- Anyone with access enters their own time on tasks they are assigned to.
-- Edit enters time on any task, for themselves or any active member.
CREATE POLICY insert_draft_time ON vault.time_entries FOR INSERT
    WITH CHECK (
        organization_id = vault.context_id('app.current_organization_id')
        AND status = 'draft'
        AND EXISTS (
            SELECT FROM vault.projects AS project
            WHERE project.organization_id = time_entries.organization_id
              AND project.id = time_entries.project_id AND NOT project.archived
        )
        AND EXISTS (
            SELECT FROM vault.tasks AS task
            WHERE task.organization_id = time_entries.organization_id
              AND task.project_id = time_entries.project_id AND task.id = time_entries.task_id
        )
        AND (
            assignment_id IS NULL OR EXISTS (
                SELECT FROM vault.assignments AS assignment
                WHERE assignment.organization_id = time_entries.organization_id
                  AND assignment.project_id = time_entries.project_id
                  AND assignment.id = time_entries.assignment_id
                  AND assignment.task_id = time_entries.task_id
            )
        )
        AND (
            (vault.has_project_role(organization_id, project_id, ARRAY['editor'])
             AND EXISTS (
                 SELECT FROM vault.organization_members AS member
                 WHERE member.organization_id = time_entries.organization_id
                   AND member.user_id = time_entries.user_id AND member.active
             ))
            OR (
                user_id = vault.context_id('app.current_user_id')
                AND vault.has_project_role(organization_id, project_id, ARRAY['viewer'])
                AND EXISTS (
                    SELECT FROM vault.assignments AS assignment
                    JOIN vault.resource_user_mappings AS mapping
                      ON mapping.organization_id = assignment.organization_id
                     AND mapping.project_id = assignment.project_id
                     AND mapping.resource_id = assignment.resource_id
                    WHERE assignment.organization_id = time_entries.organization_id
                      AND assignment.project_id = time_entries.project_id
                      AND assignment.task_id = time_entries.task_id
                      AND NOT assignment.retired
                      AND (time_entries.assignment_id IS NULL OR assignment.id = time_entries.assignment_id)
                      AND mapping.user_id = time_entries.user_id
                      AND mapping.effective_from <= transaction_timestamp()
                      AND (mapping.effective_until IS NULL OR mapping.effective_until > transaction_timestamp())
                )
            )
        )
    );

DO $migration$
BEGIN
    -- FORCE RLS applies to the migration owner too. Allow the trigger's owner
    -- to append the event it derives from a successful time entry write.
    EXECUTE format(
        'CREATE POLICY append_time_entry_events ON vault.time_entry_events
         FOR INSERT TO %I WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
         )',
        current_user
    );
    EXECUTE format(
        'CREATE POLICY manage_resource_mappings ON vault.resource_user_mappings
         FOR ALL TO %I USING (vault.can_write_schedule(organization_id, project_id))
         WITH CHECK (vault.can_write_schedule(organization_id, project_id))', current_user);
    EXECUTE format(
        'CREATE POLICY append_resource_mapping_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''resource.mapping_changed''
         )', current_user);
    EXECUTE format(
        'CREATE POLICY inspect_active_owner_for_mapping ON vault.organization_members FOR SELECT TO %I
         USING (organization_id = vault.context_id(''app.current_organization_id'')
                AND role = ''owner'' AND active)', current_user);
END
$migration$;
