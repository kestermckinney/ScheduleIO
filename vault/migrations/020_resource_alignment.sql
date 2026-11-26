-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY manage_resource_mappings ON vault.resource_user_mappings
         FOR ALL TO %I USING (vault.can_write_schedule(organization_id, project_id))
         WITH CHECK (vault.can_write_schedule(organization_id, project_id))', current_user);
    EXECUTE format(
        'CREATE POLICY append_resource_mapping_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''resource.mapping_changed''
         )', current_user);
END
$migration$;

CREATE FUNCTION vault.manager_set_resource_user(
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
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_write_schedule(tenant, project), false) THEN
            RAISE EXCEPTION 'Project unavailable for resource mapping' USING ERRCODE = 'insufficient_privilege';
        END IF;

        SELECT NOT row.retired AND row.properties->>'kind' = 'work' INTO eligible
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

REVOKE ALL ON FUNCTION vault.manager_set_resource_user(uuid, uuid, uuid, uuid) FROM PUBLIC;
