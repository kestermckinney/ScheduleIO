-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.manager_list_group_roster(tenant uuid, selected_group uuid)
    RETURNS TABLE (group_id uuid, group_name text, member_id uuid, member_name text, member_active boolean)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        roster record;
        organization_name text;
        membership_active boolean;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        -- The directory checks both the operator capability and organization role.
        FOR roster IN SELECT * FROM vault.manager_list_groups(tenant, selected_group) LOOP
            group_id := roster.group_id;
            group_name := roster.group_name;
            member_id := roster.member_id;
            member_name := NULL;
            member_active := NULL;

            IF member_id IS NOT NULL THEN
                SELECT membership.display_name, membership.active
                INTO organization_name, membership_active
                FROM vault.organization_members AS membership
                WHERE membership.organization_id = tenant AND membership.user_id = roster.member_id;
                PERFORM set_config('app.current_user_id', member_id::text, true);
                SELECT identity.display_name INTO member_name
                FROM vault.users AS identity WHERE identity.id = roster.member_id;
                PERFORM set_config('app.current_user_id', actor::text, true);
                member_active := coalesce(membership_active, false) AND member_name IS NOT NULL;
                member_name := coalesce(nullif(organization_name, ''), member_name);
            END IF;
            RETURN NEXT;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
