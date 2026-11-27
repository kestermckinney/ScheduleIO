-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- Project access is either Edit ('editor') or View ('viewer').
--   View: open the schedule and see it, and enter one's own time on tasks
--         one is assigned to (through a resource mapped to them).
--   Edit: everything View allows, plus saving the schedule, sharing it,
--         entering time on any task for any member, and approving time.
-- Organization owners and administrators keep Edit access to every project.
--
-- The six earlier roles collapse into these two: manager, editor,
-- contributor, and time_approver become Edit; viewer and time_entry become
-- View. Earlier migrations still ask for the old role names, so
-- has_project_role translates each requested name into the access that now
-- satisfies it.

-- ── Existing grants ──
ALTER TABLE vault.project_members NO FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.project_group_shares NO FORCE ROW LEVEL SECURITY;

ALTER TABLE vault.project_members DROP CONSTRAINT project_members_role_check;
ALTER TABLE vault.project_group_shares DROP CONSTRAINT project_group_shares_role_check;

INSERT INTO vault.project_members (organization_id, project_id, user_id, role)
SELECT organization_id, project_id, user_id,
       CASE WHEN role IN ('viewer', 'time_entry') THEN 'viewer' ELSE 'editor' END
FROM vault.project_members WHERE role NOT IN ('editor', 'viewer')
ON CONFLICT DO NOTHING;
DELETE FROM vault.project_members WHERE role NOT IN ('editor', 'viewer');
DELETE FROM vault.project_members AS viewing
WHERE viewing.role = 'viewer' AND EXISTS (
    SELECT FROM vault.project_members AS editing
    WHERE (editing.organization_id, editing.project_id, editing.user_id)
        = (viewing.organization_id, viewing.project_id, viewing.user_id)
      AND editing.role = 'editor');

INSERT INTO vault.project_group_shares (organization_id, project_id, group_id, role)
SELECT organization_id, project_id, group_id,
       CASE WHEN role IN ('viewer', 'time_entry') THEN 'viewer' ELSE 'editor' END
FROM vault.project_group_shares WHERE role NOT IN ('editor', 'viewer')
ON CONFLICT DO NOTHING;
DELETE FROM vault.project_group_shares WHERE role NOT IN ('editor', 'viewer');
DELETE FROM vault.project_group_shares AS viewing
WHERE viewing.role = 'viewer' AND EXISTS (
    SELECT FROM vault.project_group_shares AS editing
    WHERE (editing.organization_id, editing.project_id, editing.group_id)
        = (viewing.organization_id, viewing.project_id, viewing.group_id)
      AND editing.role = 'editor');

ALTER TABLE vault.project_members
    ADD CONSTRAINT project_members_role_check CHECK (role IN ('editor', 'viewer'));
ALTER TABLE vault.project_group_shares
    ADD CONSTRAINT project_group_shares_role_check CHECK (role IN ('editor', 'viewer'));

ALTER TABLE vault.project_members FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.project_group_shares FORCE ROW LEVEL SECURITY;

-- ── Role checks ──
-- The access that satisfies a request for any of `requested`. A request for
-- no roles stays empty, so only organization administrators pass it.
CREATE FUNCTION vault.project_access_granting(requested text[]) RETURNS text[]
    LANGUAGE sql IMMUTABLE
    SET search_path = pg_catalog
    AS $$
        SELECT CASE
            WHEN requested && ARRAY['viewer'] THEN ARRAY['editor', 'viewer']
            WHEN requested && ARRAY['editor', 'manager', 'contributor', 'time_approver', 'time_entry']
                THEN ARRAY['editor']
            ELSE ARRAY[]::text[]
        END
    $$;

CREATE OR REPLACE FUNCTION vault.has_project_role(tenant uuid, project uuid, roles text[])
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT tenant = vault.context_id('app.current_organization_id') AND (
            EXISTS (
                SELECT FROM vault.organization_members AS member
                WHERE member.organization_id = tenant
                  AND member.user_id = vault.context_id('app.current_user_id')
                  AND member.role IN ('owner', 'admin')
            )
            OR EXISTS (
                SELECT FROM vault.project_members AS grant_row
                WHERE grant_row.organization_id = tenant AND grant_row.project_id = project
                  AND grant_row.user_id = vault.context_id('app.current_user_id')
                  AND grant_row.role = ANY(vault.project_access_granting(roles))
            )
            OR EXISTS (
                SELECT FROM vault.project_group_shares AS grant_row
                WHERE grant_row.organization_id = tenant AND grant_row.project_id = project
                  AND EXISTS (
                      SELECT FROM vault.group_members AS member
                      WHERE (member.organization_id, member.group_id) = (grant_row.organization_id, grant_row.group_id)
                        AND member.user_id = vault.context_id('app.current_user_id')
                  )
                  AND grant_row.role = ANY(vault.project_access_granting(roles))
            )
        )
    $$;

-- ── View opens the whole schedule ──
ALTER POLICY read_full_schedule_data ON vault.schedule_data
    USING (vault.has_project_role(organization_id, project_id, ARRAY['viewer']));
ALTER POLICY read_live_resources_for_managers ON vault.resources
    USING (NOT retired AND vault.has_project_role(organization_id, project_id, ARRAY['viewer']));
ALTER POLICY read_live_assignments ON vault.assignments
    USING (NOT retired AND vault.has_project_role(organization_id, project_id, ARRAY['viewer']));

CREATE OR REPLACE FUNCTION vault.desktop_read_schedule(tenant uuid, project uuid)
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

-- ── Time ──
-- A person sees their own time; Edit sees everyone's on the project.
CREATE OR REPLACE FUNCTION vault.can_read_time_entry(tenant uuid, project uuid, logger uuid)
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

-- Anyone with access enters their own time on tasks they are assigned to.
-- Edit enters time on any task, for themselves or any active member.
DROP POLICY insert_own_draft_time ON vault.time_entries;
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

-- People change and submit their own drafts; Edit can do the same for anyone
-- on the project, and approves or returns other people's submitted time.
CREATE OR REPLACE FUNCTION vault.guard_time_entry_update() RETURNS trigger
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

ALTER POLICY update_visible_time_entries ON vault.time_entries
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

-- Time entered by someone with Edit for another member. Anyone allowed
-- desktop_create_time may call it; the insert policy decides the rest.
CREATE FUNCTION vault.desktop_create_time_for(
    tenant uuid, project uuid, entry_id uuid, worker uuid, task uuid, assignment uuid,
    day date, millis bigint, notes text, is_billable boolean)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        IF NOT has_function_privilege(session_user,
                'vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Time entry permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        INSERT INTO vault.time_entries
            (organization_id, project_id, id, user_id, task_id, assignment_id,
             work_date, work_millis, comments, billable)
        VALUES (tenant, project, entry_id, coalesce(worker, vault.context_id('app.current_user_id')),
                task, assignment, day, millis, notes, is_billable);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN entry_id;
    END
    $$;

CREATE OR REPLACE FUNCTION vault.desktop_list_time_actions(tenant uuid, project uuid)
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

-- ── Sharing ──
-- A principal holds one access level per project: granting Edit or View
-- replaces the other. The old role names are still accepted, so earlier
-- security exports import, and map to the access that replaced them.
CREATE OR REPLACE FUNCTION vault.manager_change_project_share(
    tenant uuid, project uuid, principal_kind text, principal_id uuid, project_role text,
    grant_access boolean)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        access text;
        changed boolean;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_manage_project_sharing(tenant, project), false) THEN
            RAISE EXCEPTION 'Edit access or organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        access := CASE
            WHEN project_role IN ('editor', 'manager', 'contributor', 'time_approver') THEN 'editor'
            WHEN project_role IN ('viewer', 'time_entry') THEN 'viewer'
        END;
        IF principal_kind IS NULL OR principal_kind NOT IN ('user', 'group')
            OR principal_id IS NULL OR grant_access IS NULL OR access IS NULL THEN
            RAISE EXCEPTION 'Invalid project access parameters' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        IF principal_kind = 'user' AND grant_access THEN
            DELETE FROM vault.project_members
            WHERE organization_id = tenant AND project_id = project
              AND user_id = principal_id AND role <> access;
            INSERT INTO vault.project_members (organization_id, project_id, user_id, role)
            VALUES (tenant, project, principal_id, access)
            ON CONFLICT DO NOTHING;
        ELSIF principal_kind = 'group' AND grant_access THEN
            DELETE FROM vault.project_group_shares
            WHERE organization_id = tenant AND project_id = project
              AND group_id = principal_id AND role <> access;
            INSERT INTO vault.project_group_shares (organization_id, project_id, group_id, role)
            VALUES (tenant, project, principal_id, access)
            ON CONFLICT DO NOTHING;
        ELSIF principal_kind = 'user' THEN
            DELETE FROM vault.project_members
            WHERE organization_id = tenant AND project_id = project
              AND user_id = principal_id AND role = access;
        ELSE
            DELETE FROM vault.project_group_shares
            WHERE organization_id = tenant AND project_id = project
              AND group_id = principal_id AND role = access;
        END IF;

        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, project_id, actor_id, operation, details)
            VALUES (tenant, project, vault.context_id('app.current_user_id'),
                    CASE WHEN grant_access THEN 'project.access_granted' ELSE 'project.access_revoked' END,
                    jsonb_build_object('principal_kind', principal_kind, 'principal_id', principal_id,
                                       'role', access));
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;
