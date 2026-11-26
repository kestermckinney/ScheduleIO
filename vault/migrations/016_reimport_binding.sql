-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY bind_reimport_digest ON vault.project_import_sources FOR INSERT TO %I
         WITH CHECK (
             source_namespace = ''sha256''
             AND source_key ~ ''^[0-9a-f]{64}$''
             AND vault.can_write_schedule(organization_id, project_id)
         )', current_user);
END
$migration$;

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
