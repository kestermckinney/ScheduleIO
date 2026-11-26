-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.manager_list_resources(tenant uuid, project uuid)
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
        SELECT resource.id, resource.properties->>'name', resource.properties->>'kind', mapping.user_id
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

REVOKE ALL ON FUNCTION vault.manager_list_resources(uuid, uuid) FROM PUBLIC;
