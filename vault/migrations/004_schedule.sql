-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.keep_assignment_endpoints() RETURNS trigger
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        IF NEW.task_id IS DISTINCT FROM OLD.task_id
            OR NEW.resource_id IS DISTINCT FROM OLD.resource_id THEN
            RAISE EXCEPTION 'Assignment endpoints cannot change' USING ERRCODE = 'check_violation';
        END IF;
        RETURN NEW;
    END
    $$;

-- Recheck lifecycle state on the row actually updated so a concurrent archive
-- cannot race a save that already checked its caller's authorization.
CREATE FUNCTION vault.begin_schedule_update(tenant uuid, project uuid, expected_edit_counter bigint,
                                                       new_title text)
    RETURNS bigint
    LANGUAGE sql VOLATILE
    SET search_path = pg_catalog
    AS $$
        UPDATE vault.projects
           SET edit_counter = edit_counter + 1, title = new_title
         WHERE organization_id = tenant AND id = project AND NOT archived
           AND edit_counter = expected_edit_counter
         RETURNING edit_counter
    $$;

REVOKE ALL ON FUNCTION vault.begin_schedule_update(uuid, uuid, bigint, text) FROM PUBLIC;

CREATE FUNCTION vault.guard_project_edit_counter() RETURNS trigger
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        IF NEW.edit_counter <> OLD.edit_counter + 1 THEN
            RAISE EXCEPTION 'Project edit counter must advance by one'
                USING ERRCODE = 'check_violation';
        END IF;
        RETURN NEW;
    END
    $$;

CREATE FUNCTION vault.record_schedule_update() RETURNS trigger
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    BEGIN
        INSERT INTO vault.audit_events (organization_id, project_id, actor_id, operation, details)
        VALUES (NEW.organization_id, NEW.id, vault.context_id('app.current_user_id'),
                'schedule.updated', jsonb_build_object('edit_counter', NEW.edit_counter));
        RETURN NULL;
    END
    $$;

REVOKE ALL ON FUNCTION vault.record_schedule_update() FROM PUBLIC;

-- Internal payload operation shared by first import and live updates.
CREATE FUNCTION vault.write_schedule_payload(
    tenant uuid, project uuid, metadata bytea, collections bytea,
    task_rows jsonb, resource_rows jsonb, assignment_rows jsonb)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        INSERT INTO vault.schedule_data
            (organization_id, project_id, format_revision, project_metadata, project_collections, properties)
        VALUES (tenant, project, 1, metadata, collections, '{}')
        ON CONFLICT (organization_id, project_id) DO UPDATE SET
            format_revision = EXCLUDED.format_revision,
            project_metadata = EXCLUDED.project_metadata,
            project_collections = EXCLUDED.project_collections;

        UPDATE vault.assignments AS current_row
        SET retired = true, model_data = NULL, properties = NULL, position = NULL
        WHERE organization_id = tenant AND project_id = project AND NOT retired
          AND NOT EXISTS (
              SELECT FROM jsonb_to_recordset(assignment_rows) AS incoming(id uuid)
              WHERE incoming.id = current_row.id
          );

        UPDATE vault.tasks AS current_row
        SET retired = true, parent_id = NULL, model_data = NULL, properties = NULL, position = NULL
        WHERE organization_id = tenant AND project_id = project AND NOT retired
          AND NOT EXISTS (
              SELECT FROM jsonb_to_recordset(task_rows) AS incoming(id uuid)
              WHERE incoming.id = current_row.id
          );

        UPDATE vault.resources AS current_row
        SET retired = true, model_data = NULL, properties = NULL, position = NULL
        WHERE organization_id = tenant AND project_id = project AND NOT retired
          AND NOT EXISTS (
              SELECT FROM jsonb_to_recordset(resource_rows) AS incoming(id uuid)
              WHERE incoming.id = current_row.id
          );

        INSERT INTO vault.tasks
            (organization_id, project_id, id, source_uid, parent_id, position, properties, model_data)
        SELECT tenant, project, id, uid, parent_id, position, properties, decode(model_data, 'base64')
        FROM jsonb_to_recordset(task_rows)
            AS incoming(id uuid, uid integer, parent_id uuid, position integer, properties jsonb, model_data text)
        ON CONFLICT (organization_id, project_id, id) DO UPDATE SET
            source_uid = EXCLUDED.source_uid, parent_id = EXCLUDED.parent_id, position = EXCLUDED.position,
            properties = EXCLUDED.properties, model_data = EXCLUDED.model_data;

        INSERT INTO vault.resources
            (organization_id, project_id, id, source_uid, position, properties, model_data)
        SELECT tenant, project, id, uid, position, properties, decode(model_data, 'base64')
        FROM jsonb_to_recordset(resource_rows)
            AS incoming(id uuid, uid integer, position integer, properties jsonb, model_data text)
        ON CONFLICT (organization_id, project_id, id) DO UPDATE SET
            source_uid = EXCLUDED.source_uid, position = EXCLUDED.position,
            properties = EXCLUDED.properties, model_data = EXCLUDED.model_data;

        INSERT INTO vault.assignments
            (organization_id, project_id, id, source_uid, task_id, resource_id, position, properties, model_data)
        SELECT tenant, project, id, uid, task_id, resource_id, position, properties, decode(model_data, 'base64')
        FROM jsonb_to_recordset(assignment_rows)
            AS incoming(id uuid, uid integer, task_id uuid, resource_id uuid, position integer,
                        properties jsonb, model_data text)
        ON CONFLICT (organization_id, project_id, id) DO UPDATE SET
            source_uid = EXCLUDED.source_uid, task_id = EXCLUDED.task_id, resource_id = EXCLUDED.resource_id,
            position = EXCLUDED.position, properties = EXCLUDED.properties, model_data = EXCLUDED.model_data;

    END
    $$;

REVOKE ALL ON FUNCTION vault.write_schedule_payload(uuid, uuid, bytea, bytea, jsonb, jsonb, jsonb)
    FROM PUBLIC;

-- Trusted schedule writers submit the already reconciled Vault IDs. The
-- invoker must hold the separate ScheduleVault write service grants.
CREATE FUNCTION vault.save_schedule(
    tenant uuid, project uuid, expected_counter bigint, title text,
    metadata bytea, collections bytea, task_rows jsonb, resource_rows jsonb,
    assignment_rows jsonb)
    RETURNS bigint
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        new_counter bigint;
    BEGIN
        new_counter := vault.begin_schedule_update(tenant, project, expected_counter, title);
        IF new_counter IS NULL THEN
            RETURN NULL;
        END IF;

        PERFORM vault.write_schedule_payload(
            tenant, project, metadata, collections, task_rows, resource_rows, assignment_rows);
        RETURN new_counter;
    END
    $$;

REVOKE ALL ON FUNCTION vault.save_schedule(uuid, uuid, bigint, text, bytea, bytea, jsonb, jsonb, jsonb)
    FROM PUBLIC;

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
                  AND vault.has_project_role(tenant, project, ARRAY['viewer'])
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

REVOKE ALL ON FUNCTION vault.desktop_read_schedule(uuid, uuid) FROM PUBLIC;

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

REVOKE ALL ON FUNCTION vault.desktop_save_schedule(uuid, uuid, bigint, text, bytea, bytea, jsonb, jsonb, jsonb)
    FROM PUBLIC;

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

CREATE FUNCTION vault.desktop_organization_work_date(tenant uuid) RETURNS date
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        today date;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.desktop_list_time(uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Vault Time permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        SELECT (statement_timestamp() AT TIME ZONE organization.time_zone)::date INTO today
        FROM vault.organizations AS organization WHERE organization.id = tenant;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN today;
    END
    $$;

CREATE TRIGGER keep_assignment_endpoints
    BEFORE UPDATE ON vault.assignments
    FOR EACH ROW EXECUTE FUNCTION vault.keep_assignment_endpoints();

CREATE TRIGGER guard_project_edit_counter
    BEFORE UPDATE OF edit_counter, title ON vault.projects
    FOR EACH ROW EXECUTE FUNCTION vault.guard_project_edit_counter();

CREATE TRIGGER record_schedule_update
    AFTER UPDATE OF edit_counter ON vault.projects
    FOR EACH ROW EXECUTE FUNCTION vault.record_schedule_update();

-- Full-model reads contain unmapped resources and native presentation data.
CREATE POLICY read_full_schedule_data ON vault.schedule_data FOR SELECT
    USING (vault.has_project_role(organization_id, project_id, ARRAY['viewer']));

CREATE POLICY read_live_resources_for_managers ON vault.resources FOR SELECT
    USING (NOT retired AND vault.has_project_role(organization_id, project_id, ARRAY['viewer']));

-- The ScheduleVault write service receives UPDATE on these columns during
-- provisioning. Manager's initial-import role does not receive that grant.
CREATE POLICY update_live_project ON vault.projects FOR UPDATE
    USING (NOT archived AND vault.has_project_role(organization_id, id, ARRAY['manager']))
    WITH CHECK (NOT archived AND vault.has_project_role(organization_id, id, ARRAY['manager']));

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY append_schedule_update_audit ON vault.audit_events
         FOR INSERT TO %I WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''schedule.updated''
         )',
        current_user
    );
END
$migration$;

DO $migration$
DECLARE
    relation text;
BEGIN
    FOREACH relation IN ARRAY ARRAY['schedule_data', 'tasks', 'resources', 'assignments']
    LOOP
        EXECUTE format(
            'CREATE POLICY insert_schedule_rows ON vault.%I FOR INSERT
             WITH CHECK (vault.can_write_schedule(organization_id, project_id))', relation);
        EXECUTE format(
            'CREATE POLICY update_schedule_rows ON vault.%I FOR UPDATE
             USING (vault.can_write_schedule(organization_id, project_id))
             WITH CHECK (vault.can_write_schedule(organization_id, project_id))', relation);
    END LOOP;
    FOREACH relation IN ARRAY ARRAY['tasks', 'resources', 'assignments']
    LOOP
        EXECUTE format(
            'CREATE POLICY read_retained_schedule_identities ON vault.%I FOR SELECT
             USING (retired AND vault.can_write_schedule(organization_id, project_id))', relation);
    END LOOP;
END
$migration$;

-- pg_dump deparses a row comparison as nested AND expressions. Use the
-- equivalent flat form so restoring the policy preserves its catalog digest.
CREATE POLICY read_own_mapped_live_resources ON vault.resources FOR SELECT
    USING (
        NOT retired AND EXISTS (
            SELECT FROM vault.resource_user_mappings AS mapping
            WHERE mapping.organization_id = resources.organization_id
              AND mapping.project_id = resources.project_id
              AND mapping.resource_id = resources.id
              AND mapping.user_id = vault.context_id('app.current_user_id')
              AND mapping.effective_from <= transaction_timestamp()
              AND mapping.effective_until IS NULL
        )
    );
