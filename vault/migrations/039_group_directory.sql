-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.manager_list_groups(tenant uuid, selected_group uuid)
    RETURNS TABLE (group_id uuid, group_name text, member_id uuid)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_change_group(uuid,uuid,text,text,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Group administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        RETURN QUERY
            SELECT team.id, team.name, member.user_id
            FROM vault.groups AS team
            LEFT JOIN vault.group_members AS member
              ON (member.organization_id, member.group_id) = (team.organization_id, team.id)
             AND selected_group IS NOT NULL
            WHERE team.organization_id = tenant AND (selected_group IS NULL OR team.id = selected_group)
            ORDER BY team.name, team.id, member.user_id;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
