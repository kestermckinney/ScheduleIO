-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY inspect_active_owner_for_mapping ON vault.organization_members FOR SELECT TO %I
         USING (organization_id = vault.context_id(''app.current_organization_id'')
                AND role = ''owner'' AND active)', current_user);
END
$migration$;

CREATE FUNCTION vault.manager_list_mapping_users(tenant uuid, project uuid)
    RETURNS TABLE (user_id uuid, display_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        member_id uuid;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_resource_user(uuid,uuid,uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Resource mapping permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_write_schedule(tenant, project), false) THEN
            RAISE EXCEPTION 'Project unavailable for resource mapping' USING ERRCODE = 'insufficient_privilege';
        END IF;

        FOR member_id IN SELECT member.user_id FROM vault.organization_members AS member
                         WHERE member.organization_id = tenant AND member.active
        LOOP
            -- The active, tenant-scoped membership is established before
            -- using the identity's self-read policy to obtain its display name.
            PERFORM set_config('app.current_user_id', member_id::text, true);
            RETURN QUERY SELECT identity.id, identity.display_name
                FROM vault.users AS identity WHERE identity.id = member_id AND identity.active;
        END LOOP;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
