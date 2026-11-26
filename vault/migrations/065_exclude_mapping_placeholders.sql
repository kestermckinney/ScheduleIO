-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE OR REPLACE FUNCTION vault.manager_list_resources(tenant uuid, project uuid)
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

CREATE OR REPLACE FUNCTION vault.manager_set_resource_user(
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
