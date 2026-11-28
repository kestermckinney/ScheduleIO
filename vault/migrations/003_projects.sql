-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

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

CREATE FUNCTION vault.has_project_role(tenant uuid, project uuid, roles text[])
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

CREATE FUNCTION vault.can_write_schedule(tenant uuid, project_id_value uuid)
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT vault.has_project_role(tenant, project_id_value, ARRAY['manager'])
            AND EXISTS (
                SELECT FROM vault.projects AS project
                WHERE project.organization_id = tenant AND project.id = project_id_value
                  AND NOT project.archived
            )
    $$;

CREATE FUNCTION vault.import_initial_schedule(
    tenant uuid, new_project uuid, title text, source_namespace text, source_key text,
    content_digest text, metadata bytea, collections bytea,
    task_rows jsonb, resource_rows jsonb, assignment_rows jsonb)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        conflict_constraint text;
    BEGIN
        IF NOT coalesce(vault.has_project_role(tenant, new_project, ARRAY[]::text[]), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF content_digest IS NULL OR content_digest !~ '^[0-9a-f]{64}$'
            OR (source_namespace IS NULL) <> (source_key IS NULL)
            OR source_namespace = 'sha256' THEN
            RAISE EXCEPTION 'Invalid import source identifiers' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        INSERT INTO vault.projects (organization_id, id, title)
        VALUES (tenant, new_project, title);

        INSERT INTO vault.project_import_sources (organization_id, project_id, source_namespace, source_key)
        VALUES (tenant, new_project, 'sha256', content_digest);

        IF source_namespace IS NOT NULL THEN
            INSERT INTO vault.project_import_sources (organization_id, project_id, source_namespace, source_key)
            VALUES (tenant, new_project, source_namespace, source_key);
        END IF;

        PERFORM vault.write_schedule_payload(
            tenant, new_project, metadata, collections, task_rows, resource_rows, assignment_rows);

        INSERT INTO vault.audit_events (organization_id, project_id, actor_id, operation)
        VALUES (tenant, new_project, vault.context_id('app.current_user_id'), 'schedule.imported');

        RETURN new_project;
    EXCEPTION WHEN unique_violation THEN
        GET STACKED DIAGNOSTICS conflict_constraint = CONSTRAINT_NAME;
        IF conflict_constraint = 'project_import_sources_pkey' THEN
            RAISE EXCEPTION 'Source already registered. Update the existing project in ScheduleVault.'
                USING ERRCODE = 'unique_violation';
        END IF;
        RAISE;
    END
    $$;

REVOKE ALL ON FUNCTION vault.import_initial_schedule(
    uuid, uuid, text, text, text, text, bytea, bytea, jsonb, jsonb, jsonb) FROM PUBLIC;

CREATE FUNCTION vault.lookup_import_source(
    tenant uuid, digest text, external_namespace text, external_key text)
    RETURNS TABLE (outcome text, project_id uuid)
    LANGUAGE plpgsql STABLE SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        matches uuid[];
    BEGIN
        IF tenant IS DISTINCT FROM vault.context_id('app.current_organization_id')
            OR NOT EXISTS (
                SELECT FROM vault.organization_members AS member
                WHERE member.organization_id = tenant
                  AND member.user_id = vault.context_id('app.current_user_id')
            ) THEN
            RAISE EXCEPTION 'Active organization membership required'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        IF digest IS NULL OR digest !~ '^[0-9a-f]{64}$'
            OR (external_namespace IS NULL) <> (external_key IS NULL)
            OR external_namespace = 'sha256' THEN
            RAISE EXCEPTION 'Invalid import source identifiers' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        SELECT array_agg(DISTINCT binding.project_id) INTO matches
        FROM vault.project_import_sources AS binding
        WHERE binding.organization_id = tenant
          AND ((binding.source_namespace = 'sha256' AND binding.source_key = digest)
              OR (binding.source_namespace = external_namespace AND binding.source_key = external_key));

        IF coalesce(cardinality(matches), 0) = 0 THEN
            RETURN QUERY SELECT 'unrecognized'::text, NULL::uuid;
        ELSIF EXISTS (
            SELECT FROM unnest(matches) AS match(id)
            WHERE NOT EXISTS (
                SELECT FROM vault.projects AS project
                WHERE project.organization_id = tenant AND project.id = match.id
            )
        ) THEN
            RETURN QUERY SELECT 'restricted'::text, NULL::uuid;
        ELSIF cardinality(matches) > 1 THEN
            RETURN QUERY SELECT 'ambiguous'::text, NULL::uuid;
        ELSE
            RETURN QUERY SELECT 'matched'::text, matches[1];
        END IF;
    END
    $$;

REVOKE ALL ON FUNCTION vault.lookup_import_source(uuid, text, text, text) FROM PUBLIC;

CREATE FUNCTION vault.desktop_reimport_schedule(
    tenant uuid, project uuid, expected_counter bigint, title text,
    metadata bytea, collections bytea, task_rows jsonb, resource_rows jsonb,
    assignment_rows jsonb, content_digest text)
    RETURNS bigint
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        saved_counter bigint;
    BEGIN
        IF content_digest IS NULL OR content_digest !~ '^[0-9a-f]{64}$' THEN
            RAISE EXCEPTION 'Invalid import digest' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_write_schedule(tenant, project), false) THEN
            RAISE EXCEPTION 'Schedule unavailable for editing' USING ERRCODE = 'insufficient_privilege';
        END IF;

        saved_counter := vault.save_schedule(
            tenant, project, expected_counter, title, metadata, collections,
            task_rows, resource_rows, assignment_rows);
        IF saved_counter IS NOT NULL THEN
            INSERT INTO vault.project_import_sources (organization_id, project_id, source_namespace, source_key)
            VALUES (tenant, project, 'sha256', content_digest)
            ON CONFLICT (organization_id, source_namespace, source_key) DO NOTHING;
            IF NOT EXISTS (
                SELECT FROM vault.project_import_sources AS binding
                WHERE binding.organization_id = tenant AND binding.project_id = project
                  AND binding.source_namespace = 'sha256' AND binding.source_key = content_digest
            ) THEN
                RAISE EXCEPTION 'Source already registered to another project' USING ERRCODE = 'unique_violation';
            END IF;
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN saved_counter;
    END
    $$;

REVOKE ALL ON FUNCTION vault.desktop_reimport_schedule(
    uuid, uuid, bigint, text, bytea, bytea, jsonb, jsonb, jsonb, text) FROM PUBLIC;

-- ── Sharing ──
-- A principal holds one access level per project: granting Edit or View
-- replaces the other. The old role names are still accepted, so earlier
-- security exports import, and map to the access that replaced them.
CREATE FUNCTION vault.manager_change_project_share(
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

REVOKE ALL ON FUNCTION vault.manager_change_project_share(uuid, uuid, text, uuid, text, boolean)
    FROM PUBLIC;

CREATE FUNCTION vault.desktop_reimport_receipt(tenant uuid, project uuid, request uuid, digest text)
    RETURNS bigint
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        recorded vault.transfer_jobs%ROWTYPE;
        counter bigint;
    BEGIN
        -- Preserve the explicit desktop repeat-import grant for existing
        -- and newly provisioned logins without changing migration history.
        IF NOT has_function_privilege(session_user,
            'vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Repeat import permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_write_schedule(tenant, project), false) THEN
            RAISE EXCEPTION 'Schedule unavailable for editing' USING ERRCODE = 'insufficient_privilege';
        END IF;

        SELECT job.* INTO recorded FROM vault.transfer_jobs AS job
        WHERE job.organization_id = tenant AND job.actor_id = vault.context_id('app.current_user_id')
          AND job.request_id = request;
        IF FOUND THEN
            IF recorded.project_id IS DISTINCT FROM project OR recorded.operation <> 'import'
                OR recorded.status <> 'complete'
                OR recorded.diagnostics->0->>'digest' IS DISTINCT FROM digest THEN
                RAISE EXCEPTION 'Request is already assigned to a different transfer'
                    USING ERRCODE = 'unique_violation';
            END IF;
            counter := (recorded.diagnostics->0->>'edit_counter')::bigint;
        END IF;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN counter;
    END
    $$;

CREATE FUNCTION vault.desktop_commit_reimport(
    tenant uuid, project uuid, expected_counter bigint, title text, metadata bytea,
    collections bytea, task_rows jsonb, resource_rows jsonb, assignment_rows jsonb,
    digest text, request uuid, file_format text)
    RETURNS bigint
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        counter bigint;
    BEGIN
        IF request IS NULL OR file_format IS NULL OR file_format NOT IN ('mpp', 'mspdi') THEN
            RAISE EXCEPTION 'Invalid repeat import request' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        -- Preserve the explicit desktop repeat-import grant for existing
        -- and newly provisioned logins without changing migration history.
        IF NOT has_function_privilege(session_user,
            'vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Repeat import permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_write_schedule(tenant, project), false) THEN
            RAISE EXCEPTION 'Schedule unavailable for editing' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM pg_advisory_xact_lock(hashtextextended(
            tenant::text || ':' || vault.context_id('app.current_user_id')::text || ':' || request::text, 3));
        counter := vault.desktop_reimport_receipt(tenant, project, request, digest);

        IF counter IS NULL THEN
            counter := vault.desktop_reimport_schedule(
                tenant, project, expected_counter, title, metadata, collections,
                task_rows, resource_rows, assignment_rows, digest);
            IF counter IS NOT NULL THEN
                INSERT INTO vault.transfer_jobs
                    (organization_id, id, project_id, actor_id, request_id, operation, status, finished_at, diagnostics)
                VALUES (tenant, gen_random_uuid(), project, vault.context_id('app.current_user_id'),
                        request, 'import', 'complete', transaction_timestamp(),
                        jsonb_build_array(jsonb_build_object(
                            'file_format', file_format, 'digest', digest, 'edit_counter', counter)));
            END IF;
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN counter;
    END
    $$;

CREATE FUNCTION vault.desktop_list_projects(tenant uuid)
    RETURNS TABLE (project_id uuid, title text, archived boolean, can_open_schedule boolean)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        RETURN QUERY
            SELECT project.id, project.title, project.archived,
                   vault.has_project_role(tenant, project.id, ARRAY['manager'])
            FROM vault.projects AS project
            WHERE project.organization_id = tenant
            ORDER BY project.archived, project.title, project.id;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.desktop_list_projects(uuid) FROM PUBLIC;

CREATE FUNCTION vault.manager_set_project_archived(tenant uuid, project uuid, archive_project boolean)
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
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        UPDATE vault.projects AS item SET archived = archive_project
        WHERE item.organization_id = tenant AND item.id = project
          AND item.archived IS DISTINCT FROM archive_project;
        changed := FOUND;

        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, project_id, actor_id, operation)
            VALUES (tenant, project, vault.context_id('app.current_user_id'),
                    CASE WHEN archive_project THEN 'project.archived' ELSE 'project.restored' END);
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_set_project_archived(uuid, uuid, boolean) FROM PUBLIC;

-- During the role lookup, only ordinary grant visibility is needed. The
-- recursion guard can suppress elevated access, but never authorize it.
CREATE FUNCTION vault.can_manage_project_sharing(tenant uuid, project uuid)
    RETURNS boolean
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_guard text := coalesce(current_setting('app.checking_project_share_manager', true), '');
        permitted boolean;
    BEGIN
        IF vault.is_organization_admin(tenant) THEN
            RETURN true;
        END IF;
        IF previous_guard = 'on' THEN
            RETURN false;
        END IF;
        PERFORM set_config('app.checking_project_share_manager', 'on', true);
        permitted := vault.has_project_role(tenant, project, ARRAY['manager']);
        PERFORM set_config('app.checking_project_share_manager', previous_guard, true);
        RETURN coalesce(permitted, false);
    END
    $$;

CREATE FUNCTION vault.manager_list_project_grants(tenant uuid, project uuid)
    RETURNS TABLE (principal_kind text, principal_id uuid, principal_name text, project_role text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        grant_row record;
        resolved_name text;
    BEGIN
        IF NOT has_function_privilege(session_user,
            'vault.manager_change_project_share(uuid,uuid,text,uuid,text,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Project sharing permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_manage_project_sharing(tenant, project), false) OR NOT EXISTS (
            SELECT FROM vault.projects AS item WHERE item.organization_id = tenant AND item.id = project
        ) THEN
            RAISE EXCEPTION 'Project unavailable for sharing administration' USING ERRCODE = 'insufficient_privilege';
        END IF;
        actor := vault.context_id('app.current_user_id');

        FOR grant_row IN
            SELECT grants.user_id, grants.role, membership.display_name
            FROM vault.project_members AS grants
            LEFT JOIN vault.organization_members AS membership
              ON (membership.organization_id, membership.user_id) = (grants.organization_id, grants.user_id)
            WHERE grants.organization_id = tenant AND grants.project_id = project
        LOOP
            PERFORM set_config('app.current_user_id', grant_row.user_id::text, true);
            SELECT identity.display_name INTO resolved_name
            FROM vault.users AS identity WHERE identity.id = grant_row.user_id;
            PERFORM set_config('app.current_user_id', actor::text, true);
            principal_kind := 'user';
            principal_id := grant_row.user_id;
            principal_name := coalesce(grant_row.display_name, resolved_name);
            project_role := grant_row.role;
            RETURN NEXT;
        END LOOP;

        RETURN QUERY
            SELECT 'group'::text, shares.group_id, team.name, shares.role
            FROM vault.project_group_shares AS shares
            LEFT JOIN vault.groups AS team
              ON (team.organization_id, team.id) = (shares.organization_id, shares.group_id)
            WHERE shares.organization_id = tenant AND shares.project_id = project;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

CREATE POLICY read_own_project_grants ON vault.project_members FOR SELECT
    USING (
        organization_id = vault.context_id('app.current_organization_id')
        AND user_id = vault.context_id('app.current_user_id')
        AND EXISTS (
            SELECT 1 FROM vault.organization_members AS member
            WHERE member.organization_id = project_members.organization_id
              AND member.user_id = project_members.user_id
        )
    );

CREATE POLICY read_joined_group_grants ON vault.project_group_shares FOR SELECT
    USING (
        organization_id = vault.context_id('app.current_organization_id')
        AND EXISTS (
            SELECT 1 FROM vault.group_members AS member
            WHERE member.organization_id = project_group_shares.organization_id
              AND member.group_id = project_group_shares.group_id
              AND member.user_id = vault.context_id('app.current_user_id')
        )
    );

CREATE POLICY read_accessible_projects ON vault.projects FOR SELECT
    USING (
        organization_id = vault.context_id('app.current_organization_id')
        AND (
            EXISTS (
                SELECT 1 FROM vault.organization_members AS member
                WHERE member.organization_id = projects.organization_id
                  AND member.user_id = vault.context_id('app.current_user_id')
                  AND member.role IN ('owner', 'admin')
            )
            OR EXISTS (
                SELECT 1 FROM vault.project_members AS grant_row
                WHERE grant_row.organization_id = projects.organization_id
                  AND grant_row.project_id = projects.id
            )
            OR EXISTS (
                SELECT 1 FROM vault.project_group_shares AS grant_row
                WHERE grant_row.organization_id = projects.organization_id
                  AND grant_row.project_id = projects.id
            )
        )
    );

DO $migration$
BEGIN
    -- The owner can create projects and bindings through the narrowed import
    -- operation below. No role receives direct source-binding table access here.
    EXECUTE format(
        'CREATE POLICY create_initial_project ON vault.projects FOR INSERT TO %I
         WITH CHECK (
             NOT archived AND edit_counter = 0
             AND vault.has_project_role(organization_id, id, ARRAY[]::text[])
         )', current_user);
    EXECUTE format(
        'CREATE POLICY bind_initial_source ON vault.project_import_sources FOR INSERT TO %I
         WITH CHECK (
             vault.has_project_role(organization_id, project_id, ARRAY[]::text[])
         )', current_user);
    EXECUTE format(
        'CREATE POLICY append_initial_import_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''schedule.imported''
         )', current_user);
    EXECUTE format(
        'CREATE POLICY lookup_import_sources ON vault.project_import_sources FOR SELECT TO %I
         USING (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND EXISTS (
                 SELECT FROM vault.organization_members AS member
                 WHERE member.organization_id = project_import_sources.organization_id
                   AND member.user_id = vault.context_id(''app.current_user_id'')
             )
         )', current_user);
    EXECUTE format(
        'CREATE POLICY bind_reimport_digest ON vault.project_import_sources FOR INSERT TO %I
         WITH CHECK (
             source_namespace = ''sha256''
             AND source_key ~ ''^[0-9a-f]{64}$''
             AND vault.can_write_schedule(organization_id, project_id)
         )', current_user);
END
$migration$;

DO $migration$
DECLARE
    relation text;
BEGIN
    FOREACH relation IN ARRAY ARRAY['project_members', 'project_group_shares']
    LOOP
        EXECUTE format(
            'CREATE POLICY manage_project_shares ON vault.%I FOR ALL TO %I
             USING (vault.can_manage_project_sharing(organization_id, project_id))
             WITH CHECK (vault.can_manage_project_sharing(organization_id, project_id))',
            relation, current_user);
    END LOOP;
    EXECUTE format(
        'CREATE POLICY append_project_share_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation IN (''project.access_granted'', ''project.access_revoked'')
         )', current_user);
END
$migration$;

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY change_project_lifecycle ON vault.projects FOR UPDATE TO %I
         USING (vault.is_organization_admin(organization_id))
         WITH CHECK (vault.is_organization_admin(organization_id))', current_user);
    EXECUTE format(
        'CREATE POLICY append_project_lifecycle_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation IN (''project.archived'', ''project.restored'')
         )', current_user);
END
$migration$;
