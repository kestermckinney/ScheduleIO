-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

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
