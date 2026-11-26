-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- The owner can create projects and bindings through the narrowed import
-- operation below. No role receives direct source-binding table access here.
DO $migration$
BEGIN
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
END
$migration$;

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
