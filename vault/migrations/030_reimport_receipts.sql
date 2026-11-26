-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

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

