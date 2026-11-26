-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.manager_overview_counts()
    RETURNS TABLE (organization_count bigint, project_count bigint, active_membership_count bigint)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        tenant uuid;
        visible_projects bigint;
        active_members bigint;
        can_list_projects boolean := has_function_privilege(session_user, 'vault.desktop_list_projects(uuid)', 'EXECUTE');
        can_list_members boolean := has_function_privilege(session_user,
            'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE');
    BEGIN
        organization_count := 0;
        IF can_list_projects THEN
            project_count := 0;
        END IF;
        FOR tenant IN SELECT directory.organization_id FROM vault.manager_list_organizations() AS directory
        LOOP
            organization_count := organization_count + 1;
            PERFORM vault.bind_manager_context(tenant);
            IF can_list_projects THEN
                SELECT count(*) INTO visible_projects
                FROM vault.projects AS entry WHERE entry.organization_id = tenant;
                project_count := project_count + visible_projects;
            END IF;
            IF can_list_members AND coalesce(vault.is_organization_admin(tenant), false) THEN
                SELECT count(*) INTO active_members FROM vault.organization_members AS member
                WHERE member.organization_id = tenant AND member.active;
                active_membership_count := coalesce(active_membership_count, 0) + active_members;
            END IF;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN NEXT;
    END
    $$;
