-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.desktop_portfolio_work(tenant uuid, team uuid DEFAULT NULL)
    RETURNS TABLE (
        project_id uuid, project_title text, user_id uuid, planned_millis numeric,
        imported_actual_millis numeric, remaining_millis numeric, approved_millis numeric)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM set_config('app.portfolio_group', '', true);
        PERFORM vault.bind_manager_context(tenant);
        IF team IS NOT NULL THEN
            IF NOT coalesce(vault.is_organization_admin(tenant), false) AND NOT (
                EXISTS (
                    SELECT FROM vault.group_members AS member
                    WHERE member.organization_id = tenant AND member.group_id = team
                      AND member.user_id = vault.context_id('app.current_user_id')
                )
                AND EXISTS (
                    SELECT FROM vault.projects AS project
                    WHERE project.organization_id = tenant AND NOT project.archived
                      AND vault.has_project_role(tenant, project.id, ARRAY['manager'])
                )
            ) THEN
                RAISE EXCEPTION 'Group reporting requires a managed project and membership in the group'
                    USING ERRCODE = 'insufficient_privilege';
            END IF;
            PERFORM set_config('app.portfolio_group', team::text, true);
        END IF;

        RETURN QUERY
            WITH authorized_projects AS (
                SELECT project.id, project.title
                FROM vault.projects AS project
                WHERE project.organization_id = tenant AND NOT project.archived
                  AND vault.has_project_role(tenant, project.id, ARRAY['manager'])
            ), report_users AS (
                SELECT member.user_id FROM vault.group_members AS member
                WHERE member.organization_id = tenant AND member.group_id = team
            ), planned AS (
                SELECT project.id, project.title, mapping.user_id,
                       sum((assignment.properties->>'work_millis')::numeric) AS planned,
                       sum((assignment.properties->>'actual_work_millis')::numeric) AS imported_actual,
                       sum((assignment.properties->>'remaining_work_millis')::numeric) AS remaining,
                       0::numeric AS approved
                FROM authorized_projects AS project
                JOIN vault.assignments AS assignment
                  ON assignment.organization_id = tenant AND assignment.project_id = project.id
                 AND NOT assignment.retired AND assignment.properties ? 'work_millis'
                JOIN vault.resources AS resource
                  ON resource.organization_id = tenant AND resource.project_id = project.id
                 AND resource.id = assignment.resource_id AND NOT resource.retired
                 AND resource.properties->>'kind' = 'work'
                LEFT JOIN vault.resource_user_mappings AS mapping
                  ON mapping.organization_id = tenant AND mapping.project_id = project.id
                 AND mapping.resource_id = resource.id AND mapping.effective_until IS NULL
                 AND mapping.effective_from <= transaction_timestamp()
                WHERE team IS NULL OR mapping.user_id IN (SELECT member.user_id FROM report_users AS member)
                GROUP BY project.id, project.title, mapping.user_id
            ), reported_time AS (
                SELECT project.id, project.title, entry.user_id, 0::numeric, 0::numeric,
                       0::numeric, sum(entry.work_millis)::numeric
                FROM authorized_projects AS project
                JOIN vault.time_entries AS entry ON entry.organization_id = tenant
                 AND entry.project_id = project.id AND entry.status = 'approved'
                WHERE team IS NULL OR entry.user_id IN (SELECT member.user_id FROM report_users AS member)
                GROUP BY project.id, project.title, entry.user_id
            )
            SELECT totals.id, totals.title, totals.user_id, sum(totals.planned),
                   sum(totals.imported_actual), sum(totals.remaining), sum(totals.approved)
            FROM (SELECT * FROM planned UNION ALL SELECT * FROM reported_time) AS totals
            GROUP BY totals.id, totals.title, totals.user_id
            ORDER BY totals.title, totals.id, totals.user_id NULLS LAST;

        PERFORM set_config('app.portfolio_group', '', true);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.desktop_portfolio_work(uuid, uuid) FROM PUBLIC;

CREATE FUNCTION vault.manager_list_audit(tenant uuid, selected_project uuid, requested_limit integer)
    RETURNS TABLE (occurred_at timestamptz, project_id uuid, actor_id uuid, operation text, details jsonb)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.has_project_role(tenant, selected_project, ARRAY['manager']), false) THEN
            RAISE EXCEPTION 'Vault audit access requires an organization admin or project manager'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        RETURN QUERY
            SELECT note.event_at, note.project, note.actor, note.action, note.payload
            FROM (
                SELECT audit.occurred_at AS event_at, audit.project_id AS project,
                       audit.actor_id AS actor, audit.operation AS action, audit.details AS payload,
                       0 AS source, audit.id AS seq
                FROM vault.audit_events AS audit
                WHERE audit.organization_id = tenant
                  AND (selected_project IS NULL OR audit.project_id = selected_project)
                UNION ALL
                SELECT event.occurred_at, event.project_id, event.actor_id,
                       'time.' || event.event_type, jsonb_build_object('entry_id', event.entry_id), 1, event.id
                FROM vault.time_entry_events AS event
                WHERE event.organization_id = tenant
                  AND (selected_project IS NULL OR event.project_id = selected_project)
                  AND vault.has_project_role(tenant, event.project_id, ARRAY['manager'])
            ) AS note
            ORDER BY note.event_at DESC, note.source, note.seq DESC
            LIMIT least(200, greatest(1, coalesce(requested_limit, 100)));

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_list_audit(uuid, uuid, integer) FROM PUBLIC;

CREATE FUNCTION vault.manager_record_transfer(
    tenant uuid, job uuid, project uuid, transfer text, completed boolean, file_format text)
    RETURNS void
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF transfer IS NULL OR transfer NOT IN ('import', 'export')
            OR completed IS NULL OR file_format IS NULL OR file_format NOT IN ('mpp', 'mspdi')
            OR (project IS NULL AND (transfer = 'export' OR completed)) THEN
            RAISE EXCEPTION 'Invalid transfer history entry' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        IF NOT coalesce(vault.has_project_role(tenant, project, ARRAY['manager']), false) THEN
            RAISE EXCEPTION 'Transfer history is unavailable' USING ERRCODE = 'insufficient_privilege';
        END IF;

        INSERT INTO vault.transfer_jobs
            (organization_id, id, project_id, actor_id, request_id, operation, status, finished_at, diagnostics)
        VALUES (tenant, job, project, vault.context_id('app.current_user_id'), job, transfer,
                CASE WHEN completed THEN 'complete' ELSE 'failed' END,
                transaction_timestamp(), jsonb_build_array(jsonb_build_object('file_format', file_format)))
        ON CONFLICT (organization_id, actor_id, request_id) DO NOTHING;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_record_transfer(uuid, uuid, uuid, text, boolean, text) FROM PUBLIC;

CREATE FUNCTION vault.manager_list_transfers(tenant uuid)
    RETURNS TABLE (project_id uuid, operation text, status text, finished_at timestamptz, file_format text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        RETURN QUERY SELECT job.project_id, job.operation, job.status, job.finished_at,
                            job.diagnostics->0->>'file_format'
        FROM vault.transfer_jobs AS job
        WHERE job.organization_id = tenant AND job.finished_at IS NOT NULL
        ORDER BY job.finished_at DESC, job.id
        LIMIT 100;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_list_transfers(uuid) FROM PUBLIC;

CREATE FUNCTION vault.manager_check_project_access(tenant uuid, project uuid, target_user uuid)
    RETURNS TABLE (project_visible boolean, full_schedule_visible boolean, schedule_writable boolean,
                   can_review_time boolean)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        operator_id uuid;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF target_user IS NULL OR NOT EXISTS (
            SELECT FROM vault.projects AS item WHERE item.organization_id = tenant AND item.id = project
        ) THEN
            RAISE EXCEPTION 'Select a project in this organization and a user'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        operator_id := vault.context_id('app.current_user_id');

        PERFORM set_config('app.current_user_id', target_user::text, true);
        SELECT EXISTS (
            SELECT FROM vault.projects AS item WHERE item.organization_id = tenant AND item.id = project
        ), EXISTS (
            SELECT FROM vault.schedule_data AS schedule
            WHERE schedule.organization_id = tenant AND schedule.project_id = project
        ), coalesce(vault.can_write_schedule(tenant, project), false),
           coalesce(vault.has_project_role(tenant, project, ARRAY['manager', 'time_approver']), false)
        INTO project_visible, full_schedule_visible, schedule_writable, can_review_time;

        PERFORM set_config('app.current_user_id', operator_id::text, true);
        INSERT INTO vault.audit_events (organization_id, project_id, actor_id, operation, details)
        VALUES (tenant, project, operator_id, 'security.project_access_checked',
                jsonb_build_object('target_user', target_user));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN NEXT;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_check_project_access(uuid, uuid, uuid) FROM PUBLIC;

CREATE FUNCTION vault.manager_export_security(tenant uuid) RETURNS jsonb
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        result jsonb;
        members jsonb := '[]'::jsonb;
        actor uuid;
        member_row record;
        identity_row record;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        IF NOT EXISTS (
            SELECT FROM vault.organization_members AS member
            WHERE member.organization_id = tenant
              AND member.user_id = vault.context_id('app.current_user_id')
              AND member.role = 'owner' AND member.active
        ) THEN
            RAISE EXCEPTION 'Organization owner required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        FOR member_row IN
            SELECT member.user_id, member.role, member.active
            FROM vault.organization_members AS member
            WHERE member.organization_id = tenant AND NOT vault.is_service_account(member.user_id)
            ORDER BY member.user_id
        LOOP
            -- Resolve only identities that belong to the selected organization.
            PERFORM set_config('app.current_user_id', member_row.user_id::text, true);
            SELECT identity.issuer,
                   CASE WHEN identity.issuer = 'postgresql' THEN login.rolname::text
                        ELSE identity.subject END AS subject
            INTO identity_row
            FROM vault.users AS identity
            LEFT JOIN LATERAL (
                SELECT role.rolname
                FROM vault.manager_principals AS principal
                JOIN pg_roles AS role
                  ON (role.oid, role.rolname) = (principal.database_role_id, principal.database_role_name)
                WHERE identity.issuer = 'postgresql' AND principal.user_id = identity.id
                  AND principal.active AND role.rolcanlogin
                ORDER BY (principal.database_role_id::text = identity.subject) DESC, role.rolname
                LIMIT 1
            ) AS login ON true
            WHERE identity.id = member_row.user_id;
            members := members || jsonb_build_array(jsonb_build_object(
                'user_id', member_row.user_id, 'issuer', identity_row.issuer, 'subject', identity_row.subject,
                'role', member_row.role, 'active', member_row.active AND identity_row.issuer IS NOT NULL
            ));
        END LOOP;
        PERFORM set_config('app.current_user_id', actor::text, true);

        SELECT jsonb_build_object(
            'format', 1,
            'organization_id', tenant,
            'members', members,
            'groups', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'group_id', team.id, 'name', team.name, 'members', coalesce((
                        SELECT jsonb_agg(member.user_id ORDER BY member.user_id)
                        FROM vault.group_members AS member
                        WHERE (member.organization_id, member.group_id) = (tenant, team.id)
                    ), '[]'::jsonb)
                ) ORDER BY team.name, team.id)
                FROM vault.groups AS team WHERE team.organization_id = tenant
            ), '[]'::jsonb),
            'projects', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'project_id', project.id, 'title', project.title,
                    'user_grants', coalesce((
                        SELECT jsonb_agg(jsonb_build_object('user_id', grant_row.user_id, 'role', grant_row.role)
                                         ORDER BY grant_row.user_id, grant_row.role)
                        FROM vault.project_members AS grant_row
                        WHERE (grant_row.organization_id, grant_row.project_id) = (tenant, project.id)
                    ), '[]'::jsonb),
                    'group_grants', coalesce((
                        SELECT jsonb_agg(jsonb_build_object('group_id', grant_row.group_id, 'role', grant_row.role)
                                         ORDER BY grant_row.group_id, grant_row.role)
                        FROM vault.project_group_shares AS grant_row
                        WHERE (grant_row.organization_id, grant_row.project_id) = (tenant, project.id)
                    ), '[]'::jsonb)
                ) ORDER BY project.title, project.id)
                FROM vault.projects AS project WHERE project.organization_id = tenant
            ), '[]'::jsonb)
        ) INTO result;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN result;
    END
    $$;

CREATE FUNCTION vault.manager_preview_security_import(tenant uuid, source jsonb) RETURNS jsonb
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        destination jsonb;
    BEGIN
        IF jsonb_typeof(source) IS DISTINCT FROM 'object'
            OR source->'format' IS DISTINCT FROM '1'::jsonb
            OR jsonb_typeof(source->'members') IS DISTINCT FROM 'array'
            OR jsonb_typeof(source->'groups') IS DISTINCT FROM 'array'
            OR jsonb_typeof(source->'projects') IS DISTINCT FROM 'array'
        THEN
            RAISE EXCEPTION 'Expected a format 1 Vault security configuration'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        -- This supplies the same tenant and owner authorization as export.
        destination := vault.manager_export_security(tenant);

        RETURN jsonb_build_object(
            'destination_organization_id', tenant,
            'members', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'source_user_id', original->'user_id',
                    'destination_user_id', target->'user_id',
                    'issuer', original->'issuer',
                    'subject', original->'subject',
                    'requested_role', original->'role',
                    'requested_active', original->'active',
                    'current_role', target->'role',
                    'current_active', target->'active'
                ) ORDER BY original->>'user_id')
                FROM jsonb_array_elements(source->'members') AS original
                LEFT JOIN jsonb_array_elements(destination->'members') AS target
                    ON original->>'issuer' = target->>'issuer'
                   AND original->>'subject' = target->>'subject'
            ), '[]'::jsonb),
            'groups', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'source_group_id', original->'group_id',
                    'destination_group_id', target->'group_id',
                    'source_name', original->'name',
                    'destination_name', target->'name',
                    'requested_members', original->'members',
                    'current_members', target->'members'
                ) ORDER BY original->>'group_id')
                FROM jsonb_array_elements(source->'groups') AS original
                LEFT JOIN jsonb_array_elements(destination->'groups') AS target
                    ON source #>> ARRAY['destination_groups', original->>'group_id'] = target->>'group_id'
            ), '[]'::jsonb),
            'projects', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'source_project_id', original->'project_id',
                    'destination_project_id', target->'project_id',
                    'source_title', original->'title',
                    'destination_title', target->'title',
                    'requested_user_grants', original->'user_grants',
                    'current_user_grants', target->'user_grants',
                    'requested_group_grants', original->'group_grants',
                    'current_group_grants', target->'group_grants'
                ) ORDER BY original->>'project_id')
                FROM jsonb_array_elements(source->'projects') AS original
                LEFT JOIN jsonb_array_elements(destination->'projects') AS target
                    ON source #>> ARRAY['destination_projects', original->>'project_id'] = target->>'project_id'
            ), '[]'::jsonb)
        );
    END
    $$;

CREATE FUNCTION vault.manager_apply_security_import(tenant uuid, source jsonb, reviewed jsonb)
    RETURNS integer
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        plan jsonb;
        users_by_source jsonb;
        groups_by_source jsonb;
        item jsonb;
        grant_row jsonb;
        target uuid;
        member uuid;
        requested_role text;
        requested_active boolean;
        changes integer := 0;
    BEGIN
        IF current_setting('transaction_isolation') <> 'serializable' THEN
            RAISE EXCEPTION 'Apply security configuration in a serializable transaction'
                USING ERRCODE = 'invalid_transaction_state';
        END IF;

        plan := vault.manager_preview_security_import(tenant, source);
        IF plan IS DISTINCT FROM reviewed THEN
            RAISE EXCEPTION 'Security configuration changed. Review a new preview before applying.'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        IF NOT has_function_privilege(session_user, 'vault.manager_change_group(uuid,uuid,text,text,uuid)', 'EXECUTE')
            OR NOT has_function_privilege(session_user,
                'vault.manager_change_project_share(uuid,uuid,text,uuid,text,boolean)', 'EXECUTE')
        THEN
            RAISE EXCEPTION 'Group and project sharing administration permissions required'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        IF EXISTS (SELECT FROM jsonb_array_elements(plan->'members') AS row_data
                   WHERE row_data->>'destination_user_id' IS NULL)
            OR EXISTS (SELECT FROM jsonb_array_elements(plan->'groups') AS row_data
                       WHERE row_data->>'destination_group_id' IS NULL)
            OR EXISTS (SELECT FROM jsonb_array_elements(plan->'projects') AS row_data
                       WHERE row_data->>'destination_project_id' IS NULL)
        THEN
            RAISE EXCEPTION 'Every source member, group, and project must have a destination'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        IF (SELECT count(DISTINCT row_data->>'source_user_id') <> count(*)
                   OR count(DISTINCT row_data->>'destination_user_id') <> count(*)
            FROM jsonb_array_elements(plan->'members') AS row_data)
            OR (SELECT count(DISTINCT row_data->>'source_group_id') <> count(*)
                       OR count(DISTINCT row_data->>'destination_group_id') <> count(*)
                FROM jsonb_array_elements(plan->'groups') AS row_data)
            OR (SELECT count(DISTINCT row_data->>'source_project_id') <> count(*)
                       OR count(DISTINCT row_data->>'destination_project_id') <> count(*)
                FROM jsonb_array_elements(plan->'projects') AS row_data)
        THEN
            RAISE EXCEPTION 'Security configuration mappings must be one-to-one'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        IF EXISTS (SELECT FROM jsonb_array_elements(plan->'groups') AS row_data
                   WHERE jsonb_typeof(row_data->'requested_members') IS DISTINCT FROM 'array')
            OR EXISTS (SELECT FROM jsonb_array_elements(plan->'projects') AS row_data
                       WHERE jsonb_typeof(row_data->'requested_user_grants') IS DISTINCT FROM 'array'
                          OR jsonb_typeof(row_data->'requested_group_grants') IS DISTINCT FROM 'array')
        THEN
            RAISE EXCEPTION 'Expected arrays for group members and project grants'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        SELECT jsonb_object_agg(row_data->>'source_user_id', row_data->>'destination_user_id')
        INTO users_by_source FROM jsonb_array_elements(plan->'members') AS row_data;
        SELECT jsonb_object_agg(row_data->>'source_group_id', row_data->>'destination_group_id')
        INTO groups_by_source FROM jsonb_array_elements(plan->'groups') AS row_data;

        FOR item IN SELECT value FROM jsonb_array_elements(plan->'members') LOOP
            requested_role := item->>'requested_role';
            requested_active := (item->>'requested_active')::boolean;
            IF requested_role IS NULL OR requested_role NOT IN ('owner', 'admin', 'member')
                OR jsonb_typeof(item->'requested_active') IS DISTINCT FROM 'boolean'
            THEN
                RAISE EXCEPTION 'Invalid member role or status' USING ERRCODE = 'invalid_parameter_value';
            END IF;
            IF requested_role = 'owner' OR item->>'current_role' = 'owner' THEN
                CONTINUE;
            END IF;

            member := (item->>'destination_user_id')::uuid;
            IF requested_active AND NOT (item->>'current_active')::boolean THEN
                IF NOT vault.manager_set_member_active(tenant, member, true) THEN
                    RAISE EXCEPTION 'Could not reactivate destination member %', member;
                END IF;
                changes := changes + 1;
            END IF;
            IF requested_role <> item->>'current_role' THEN
                IF NOT vault.manager_set_member_role(tenant, member, requested_role) THEN
                    RAISE EXCEPTION 'Could not change destination member role for %', member;
                END IF;
                changes := changes + 1;
            END IF;
            IF NOT requested_active AND (item->>'current_active')::boolean THEN
                IF NOT vault.manager_set_member_active(tenant, member, false) THEN
                    RAISE EXCEPTION 'Could not deactivate destination member %', member;
                END IF;
                changes := changes + 1;
            END IF;
        END LOOP;

        FOR item IN SELECT value FROM jsonb_array_elements(plan->'groups') LOOP
            target := (item->>'destination_group_id')::uuid;
            FOR member IN
                SELECT (users_by_source->>source_id)::uuid
                FROM jsonb_array_elements_text(item->'requested_members') AS source_id
            LOOP
                IF vault.manager_change_group(tenant, target, 'add_member', NULL, member) THEN
                    changes := changes + 1;
                END IF;
            END LOOP;
        END LOOP;

        FOR item IN SELECT value FROM jsonb_array_elements(plan->'projects') LOOP
            target := (item->>'destination_project_id')::uuid;
            FOR grant_row IN SELECT value FROM jsonb_array_elements(item->'requested_user_grants') LOOP
                IF vault.manager_change_project_share(tenant, target, 'user',
                        (users_by_source->>(grant_row->>'user_id'))::uuid, grant_row->>'role', true) THEN
                    changes := changes + 1;
                END IF;
            END LOOP;
            FOR grant_row IN SELECT value FROM jsonb_array_elements(item->'requested_group_grants') LOOP
                IF vault.manager_change_project_share(tenant, target, 'group',
                        (groups_by_source->>(grant_row->>'group_id'))::uuid, grant_row->>'role', true) THEN
                    changes := changes + 1;
                END IF;
            END LOOP;
        END LOOP;

        RETURN changes;
    END
    $$;

CREATE FUNCTION vault.desktop_named_portfolio_work(tenant uuid, team uuid DEFAULT NULL)
    RETURNS TABLE (
        project_id uuid, project_title text, user_id uuid, planned_millis numeric,
        imported_actual_millis numeric, remaining_millis numeric, approved_millis numeric,
        user_display_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        item record;
        tenant_name text;
        identity_name text;
        names jsonb := '{}'::jsonb;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.desktop_portfolio_work(uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Portfolio reporting permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');

        FOR item IN SELECT * FROM vault.desktop_portfolio_work(tenant, team) LOOP
            IF item.user_id IS NOT NULL AND NOT (names ? item.user_id::text) THEN
                SELECT membership.display_name INTO tenant_name
                FROM vault.organization_members AS membership
                WHERE membership.organization_id = tenant AND membership.user_id = item.user_id;

                PERFORM set_config('app.current_user_id', item.user_id::text, true);
                SELECT identity.display_name INTO identity_name
                FROM vault.users AS identity WHERE identity.id = item.user_id;
                PERFORM set_config('app.current_user_id', actor::text, true);

                names := names || jsonb_build_object(item.user_id::text,
                    coalesce(nullif(tenant_name, ''), nullif(identity_name, ''), item.user_id::text));
            END IF;
            project_id := item.project_id;
            project_title := item.project_title;
            user_id := item.user_id;
            planned_millis := item.planned_millis;
            imported_actual_millis := item.imported_actual_millis;
            remaining_millis := item.remaining_millis;
            approved_millis := item.approved_millis;
            user_display_name := names->>item.user_id::text;
            RETURN NEXT;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

CREATE FUNCTION vault.desktop_portfolio_groups(tenant uuid)
    RETURNS TABLE (group_id uuid, group_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        previous_group text := coalesce(current_setting('app.portfolio_group', true), '');
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.desktop_portfolio_work(uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Portfolio reporting permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM set_config('app.portfolio_group', '', true);
        PERFORM vault.bind_manager_context(tenant);
        IF coalesce(vault.is_organization_admin(tenant), false) OR EXISTS (
            SELECT FROM vault.projects AS project
            WHERE project.organization_id = tenant AND NOT project.archived
              AND vault.has_project_role(tenant, project.id, ARRAY['manager'])
        ) THEN
            RETURN QUERY SELECT team.id, team.name FROM vault.groups AS team
            WHERE team.organization_id = tenant
            ORDER BY team.name, team.id;
        END IF;
        PERFORM set_config('app.portfolio_group', previous_group, true);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

CREATE FUNCTION vault.manager_overview_counts()
    RETURNS TABLE (organization_count bigint, project_count bigint, active_membership_count bigint)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        tenant uuid;
        visible_projects bigint;
        active_members bigint;
        can_list_projects boolean := has_function_privilege(session_user, 'vault.desktop_list_projects(uuid)', 'EXECUTE');
        can_list_members boolean := has_function_privilege(session_user,
            'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE');
    BEGIN
        organization_count := 0;
        IF can_list_projects THEN
            project_count := 0;
        END IF;
        FOR tenant IN SELECT directory.organization_id FROM vault.manager_list_organizations() AS directory
        LOOP
            organization_count := organization_count + 1;
            PERFORM vault.bind_manager_context(tenant);
            IF can_list_projects THEN
                SELECT count(*) INTO visible_projects
                FROM vault.projects AS entry WHERE entry.organization_id = tenant;
                project_count := project_count + visible_projects;
            END IF;
            IF can_list_members AND coalesce(vault.is_organization_admin(tenant), false) THEN
                SELECT count(*) INTO active_members FROM vault.organization_members AS member
                WHERE member.organization_id = tenant AND member.active
                  AND NOT vault.is_service_account(member.user_id);
                active_membership_count := coalesce(active_membership_count, 0) + active_members;
            END IF;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN NEXT;
    END
    $$;

CREATE POLICY read_own_transfer_jobs ON vault.transfer_jobs FOR SELECT
    USING (
        actor_id = vault.context_id('app.current_user_id')
        AND EXISTS (
            SELECT FROM vault.organization_members AS member
            WHERE member.organization_id = transfer_jobs.organization_id
              AND member.user_id = transfer_jobs.actor_id
        )
        AND (
            project_id IS NULL OR EXISTS (
                SELECT FROM vault.projects AS project
                WHERE project.organization_id = transfer_jobs.organization_id
                  AND project.id = transfer_jobs.project_id
            )
        )
    );

CREATE POLICY read_admin_audit ON vault.audit_events FOR SELECT
    USING (
        vault.has_project_role(organization_id, project_id, ARRAY['manager'])
    );

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY append_transfer_jobs ON vault.transfer_jobs FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND vault.has_project_role(organization_id, project_id, ARRAY[''manager''])
         )', current_user);
    EXECUTE format(
        'CREATE POLICY append_access_diagnostic_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''security.project_access_checked''
         )', current_user);
    -- This SELECT policy supplies the report's roster after it checks the
    -- requesting manager and group. It cannot make the actor a group member.
    EXECUTE format(
        'CREATE POLICY read_portfolio_group_roster ON vault.group_members
         FOR SELECT TO %I USING (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND group_id = vault.context_id(''app.portfolio_group'')
             AND user_id <> vault.context_id(''app.current_user_id'')
         )', current_user);
END
$migration$;
