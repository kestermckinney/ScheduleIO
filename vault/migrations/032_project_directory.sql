-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

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
