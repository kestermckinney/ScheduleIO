-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY append_transfer_jobs ON vault.transfer_jobs FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND vault.has_project_role(organization_id, project_id, ARRAY[''manager''])
         )', current_user);
END
$migration$;

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

REVOKE ALL ON FUNCTION vault.manager_record_transfer(uuid, uuid, uuid, text, boolean, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.manager_list_transfers(uuid) FROM PUBLIC;
