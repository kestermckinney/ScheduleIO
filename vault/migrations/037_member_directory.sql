-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.manager_list_members(tenant uuid)
    RETURNS TABLE (member_id uuid, display_name text, membership_role text, membership_active boolean)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        membership record;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        actor := vault.context_id('app.current_user_id');

        FOR membership IN
            SELECT member.user_id, member.role, member.active
            FROM vault.organization_members AS member
            WHERE member.organization_id = tenant
            ORDER BY NOT member.active, member.role, member.user_id
        LOOP
            -- The member row is already scoped to this organization. Use the
            -- identity self-read policy to project only its display name.
            PERFORM set_config('app.current_user_id', membership.user_id::text, true);
            SELECT identity.display_name INTO display_name
            FROM vault.users AS identity WHERE identity.id = membership.user_id;
            PERFORM set_config('app.current_user_id', actor::text, true);

            member_id := membership.user_id;
            membership_role := membership.role;
            membership_active := membership.active AND display_name IS NOT NULL;
            RETURN NEXT;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
