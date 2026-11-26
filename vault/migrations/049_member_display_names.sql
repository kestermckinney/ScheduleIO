-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE vault.organization_members ADD COLUMN display_name text
    CHECK (display_name IS NULL OR (display_name = btrim(display_name) AND char_length(display_name) BETWEEN 1 AND 200));

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY append_member_name_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''organization.member_name_changed''
         )', current_user);
END
$migration$;

CREATE FUNCTION vault.manager_set_member_name(tenant uuid, member_id uuid, new_name text)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        actor_is_owner boolean;
        changed boolean;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        actor := vault.context_id('app.current_user_id');
        SELECT member.role = 'owner' INTO actor_is_owner
        FROM vault.organization_members AS member
        WHERE member.organization_id = tenant AND member.user_id = actor AND member.active;

        IF new_name IS NULL OR char_length(btrim(new_name)) NOT BETWEEN 1 AND 200 THEN
            RAISE EXCEPTION 'Enter a display name of 1 to 200 characters' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        UPDATE vault.organization_members AS member SET display_name = btrim(new_name)
        WHERE member.organization_id = tenant AND member.user_id = member_id AND member.user_id <> actor
          AND (member.role = 'member' OR (actor_is_owner AND member.role = 'admin'))
          AND member.display_name IS DISTINCT FROM btrim(new_name);
        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, actor, 'organization.member_name_changed', jsonb_build_object('user_id', member_id));
        END IF;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;

CREATE OR REPLACE FUNCTION vault.manager_list_members(tenant uuid)
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
            SELECT member.user_id, member.role, member.active, member.display_name
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
            display_name := coalesce(membership.display_name, display_name);
            RETURN NEXT;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

CREATE OR REPLACE FUNCTION vault.manager_list_mapping_users(tenant uuid, project uuid)
    RETURNS TABLE (user_id uuid, display_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        member record;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_resource_user(uuid,uuid,uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Resource mapping permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.can_write_schedule(tenant, project), false) THEN
            RAISE EXCEPTION 'Project unavailable for resource mapping' USING ERRCODE = 'insufficient_privilege';
        END IF;

        FOR member IN SELECT membership.user_id, membership.display_name
                      FROM vault.organization_members AS membership
                      WHERE membership.organization_id = tenant AND membership.active
        LOOP
            -- The active, tenant-scoped membership is established before
            -- using the identity's self-read policy to obtain its display name.
            PERFORM set_config('app.current_user_id', member.user_id::text, true);
            RETURN QUERY SELECT identity.id, coalesce(member.display_name, identity.display_name)
                FROM vault.users AS identity WHERE identity.id = member.user_id AND identity.active;
        END LOOP;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
