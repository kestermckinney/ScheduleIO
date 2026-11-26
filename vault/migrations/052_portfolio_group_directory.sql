-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.desktop_portfolio_groups(tenant uuid)
    RETURNS TABLE (group_id uuid, group_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        previous_group text := coalesce(current_setting('app.portfolio_group', true), '');
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.desktop_portfolio_work(uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Portfolio reporting permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM set_config('app.portfolio_group', '', true);
        PERFORM vault.bind_manager_context(tenant);
        IF coalesce(vault.is_organization_admin(tenant), false) OR EXISTS (
            SELECT FROM vault.projects AS project
            WHERE project.organization_id = tenant AND NOT project.archived
              AND vault.has_project_role(tenant, project.id, ARRAY['manager'])
        ) THEN
            RETURN QUERY SELECT team.id, team.name FROM vault.groups AS team
            WHERE team.organization_id = tenant
            ORDER BY team.name, team.id;
        END IF;
        PERFORM set_config('app.portfolio_group', previous_group, true);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
