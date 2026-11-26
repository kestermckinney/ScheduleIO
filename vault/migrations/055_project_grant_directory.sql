-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.manager_list_project_grants(tenant uuid, project uuid)
    RETURNS TABLE (principal_kind text, principal_id uuid, principal_name text, project_role text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        grant_row record;
        resolved_name text;
    BEGIN
        IF NOT has_function_privilege(session_user,
            'vault.manager_change_project_share(uuid,uuid,text,uuid,text,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Project sharing permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_manage_project_sharing(tenant, project), false) OR NOT EXISTS (
            SELECT FROM vault.projects AS item WHERE item.organization_id = tenant AND item.id = project
        ) THEN
            RAISE EXCEPTION 'Project unavailable for sharing administration' USING ERRCODE = 'insufficient_privilege';
        END IF;
        actor := vault.context_id('app.current_user_id');

        FOR grant_row IN
            SELECT grants.user_id, grants.role, membership.display_name
            FROM vault.project_members AS grants
            LEFT JOIN vault.organization_members AS membership
              ON (membership.organization_id, membership.user_id) = (grants.organization_id, grants.user_id)
            WHERE grants.organization_id = tenant AND grants.project_id = project
        LOOP
            PERFORM set_config('app.current_user_id', grant_row.user_id::text, true);
            SELECT identity.display_name INTO resolved_name
            FROM vault.users AS identity WHERE identity.id = grant_row.user_id;
            PERFORM set_config('app.current_user_id', actor::text, true);
            principal_kind := 'user';
            principal_id := grant_row.user_id;
            principal_name := coalesce(grant_row.display_name, resolved_name);
            project_role := grant_row.role;
            RETURN NEXT;
        END LOOP;

        RETURN QUERY
            SELECT 'group'::text, shares.group_id, team.name, shares.role
            FROM vault.project_group_shares AS shares
            LEFT JOIN vault.groups AS team
              ON (team.organization_id, team.id) = (shares.organization_id, shares.group_id)
            WHERE shares.organization_id = tenant AND shares.project_id = project;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
