-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- A member's schedule access and time entry can change after they are added.
-- Schedule access is the right to open and save schedules shared with them;
-- sharing still decides which schedules those are.

CREATE FUNCTION vault.member_login(member uuid) RETURNS name
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT login.rolname
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login
          ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
        WHERE principal.user_id = member AND login.rolcanlogin
        ORDER BY NOT principal.active, login.rolname LIMIT 1
    $$;
REVOKE ALL ON FUNCTION vault.member_login(uuid) FROM PUBLIC;

CREATE FUNCTION vault.manager_list_member_access(tenant uuid)
    RETURNS TABLE (member_id uuid, schedule_access boolean, time_access boolean)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        membership record;
        member_login name;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        FOR membership IN
            SELECT member.user_id
            FROM vault.organization_members AS member
            WHERE member.organization_id = tenant AND NOT vault.is_service_account(member.user_id)
        LOOP
            member_login := vault.member_login(membership.user_id);
            member_id := membership.user_id;
            schedule_access := member_login IS NOT NULL AND has_function_privilege(member_login,
                'vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb)', 'EXECUTE');
            time_access := member_login IS NOT NULL AND has_function_privilege(member_login,
                'vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean)', 'EXECUTE');
            RETURN NEXT;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
REVOKE ALL ON FUNCTION vault.manager_list_member_access(uuid) FROM PUBLIC;

-- Returns true when the member's access changed.
CREATE FUNCTION vault.manager_set_member_access(tenant uuid, member uuid, allow_schedule boolean, allow_time boolean)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        member_role text;
        member_login name;
        had_schedule boolean;
        had_time boolean;
    BEGIN
        IF NOT has_function_privilege(session_user,
                'vault.manager_enroll_member(uuid,text,name,boolean,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        actor := vault.context_id('app.current_user_id');
        IF allow_schedule IS NULL OR allow_time IS NULL OR NOT (allow_schedule OR allow_time) THEN
            RAISE EXCEPTION 'Allow schedule access, time entry, or both' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        SELECT membership.role INTO member_role
        FROM vault.organization_members AS membership
        WHERE membership.organization_id = tenant AND membership.user_id = member AND membership.active;
        member_login := vault.member_login(member);
        IF member_role IS NULL OR member_login IS NULL OR vault.is_service_account(member) THEN
            RAISE EXCEPTION 'Select an active member of this organization with a Vault login'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF member_role = 'owner' THEN
            RAISE EXCEPTION 'The owner always has every feature' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        had_schedule := has_function_privilege(member_login,
            'vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb)', 'EXECUTE');
        had_time := has_function_privilege(member_login,
            'vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean)', 'EXECUTE');
        IF had_schedule = allow_schedule AND had_time = allow_time THEN
            PERFORM set_config('app.current_organization_id', previous_tenant, true);
            PERFORM set_config('app.current_user_id', previous_actor, true);
            RETURN false;
        END IF;

        IF allow_schedule AND NOT had_schedule THEN
            PERFORM vault.enable_desktop_capability(member_login, 'schedule_updates');
        ELSIF had_schedule AND NOT allow_schedule THEN
            PERFORM vault.revoke_desktop_schedule_updates(member_login::text);
            -- Exporting also reads schedules; keep reading for members who export.
            IF NOT has_function_privilege(member_login, 'vault.desktop_approved_export_work(uuid,uuid)', 'EXECUTE') THEN
                EXECUTE format('REVOKE EXECUTE ON FUNCTION vault.desktop_read_schedule(uuid,uuid) FROM %I',
                               member_login);
            END IF;
        END IF;

        IF allow_time AND NOT had_time THEN
            PERFORM vault.enable_desktop_capability(member_login, 'time');
        ELSIF had_time AND NOT allow_time THEN
            EXECUTE format(
                'REVOKE EXECUTE ON FUNCTION vault.desktop_list_time_work(uuid,uuid),
                    vault.desktop_list_time(uuid,uuid),
                    vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean),
                    vault.desktop_update_time(uuid,uuid,uuid,text,text,date,bigint,text,boolean)
                 FROM %I', member_login);
            INSERT INTO vault_admin.desktop_capability_events
                (operator_login, operator_role, target_login, capability, action)
            VALUES (session_user, current_user, member_login, 'time', 'revoked');
        END IF;

        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, actor, 'organization.member_access_changed',
                jsonb_build_object('user_id', member, 'schedule', allow_schedule, 'time', allow_time));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN true;
    END
    $$;
REVOKE ALL ON FUNCTION vault.manager_set_member_access(uuid, uuid, boolean, boolean) FROM PUBLIC;

-- ── Member administrators get the new entry points ───────────────────────

DO $migration$
DECLARE
    body text;
    login name;
BEGIN
    SELECT pg_get_functiondef('vault.grant_desktop_capability_privileges(name,text)'::regprocedure) INTO body;
    body := replace(body,
        '''vault.manager_enable_member_capability(uuid,uuid,text)''];',
        '''vault.manager_enable_member_capability(uuid,uuid,text)'',
                    ''vault.manager_list_member_access(uuid)'',
                    ''vault.manager_set_member_access(uuid,uuid,boolean,boolean)''];');
    IF body NOT LIKE '%manager_set_member_access%' THEN
        RAISE EXCEPTION 'Could not update vault.grant_desktop_capability_privileges';
    END IF;
    EXECUTE body;

    FOR login IN
        SELECT role.rolname
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS role
          ON role.oid = principal.database_role_id AND role.rolname = principal.database_role_name
        WHERE has_function_privilege(role.oid, 'vault.manager_enable_member_capability(uuid,uuid,text)', 'EXECUTE')
    LOOP
        EXECUTE format('GRANT EXECUTE ON FUNCTION vault.manager_list_member_access(uuid),
                            vault.manager_set_member_access(uuid,uuid,boolean,boolean) TO %I', login);
    END LOOP;
END
$migration$;
