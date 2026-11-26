-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY append_member_enrollment_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''organization.member_enrolled''
         )', current_user);
END
$migration$;

CREATE FUNCTION vault.manager_enroll_member(
    tenant uuid, display_name text, desktop_login name, allow_schedule boolean, allow_time boolean)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        login_id oid;
        member_id uuid;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF nullif(btrim(display_name), '') IS NULL
           OR NOT (coalesce(allow_schedule, false) OR coalesce(allow_time, false)) THEN
            RAISE EXCEPTION 'A member name and desktop capability are required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        SELECT oid INTO login_id FROM pg_roles
        WHERE rolname = desktop_login AND rolcanlogin AND NOT rolsuper
          AND NOT rolbypassrls AND NOT rolcreaterole AND NOT rolcreatedb
          AND NOT rolreplication AND NOT pg_has_role(oid, current_user, 'MEMBER');
        IF login_id IS NULL THEN
            RAISE EXCEPTION 'A separate unprivileged PostgreSQL login is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        PERFORM pg_advisory_xact_lock(173195, (login_id::text::bigint % 2147483647)::integer);
        SELECT principal.user_id INTO member_id
        FROM vault.manager_principals AS principal
        WHERE principal.database_role_id = login_id AND principal.database_role_name = desktop_login
          AND principal.active;
        IF member_id IS NULL THEN
            member_id := gen_random_uuid();
            INSERT INTO vault.users (id, issuer, subject, display_name)
            VALUES (member_id, 'postgresql', login_id::text, display_name);
            INSERT INTO vault.manager_principals (database_role_id, database_role_name, user_id)
            VALUES (login_id, desktop_login, member_id);
        END IF;

        INSERT INTO vault.organization_members (organization_id, user_id, role)
        VALUES (tenant, member_id, 'member')
        ON CONFLICT DO NOTHING;
        IF FOUND THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, vault.context_id('app.current_user_id'), 'organization.member_enrolled',
                    jsonb_build_object('user_id', member_id));
        END IF;

        EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', desktop_login);
        IF allow_schedule THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.desktop_read_schedule(uuid,uuid),
                    vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb),
                    vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text),
                    vault.manager_lookup_import_source(uuid,text,text,text) TO %I', desktop_login);
        END IF;
        IF allow_time THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.desktop_list_time_work(uuid,uuid),
                    vault.desktop_list_time(uuid,uuid),
                    vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean),
                    vault.desktop_update_time(uuid,uuid,uuid,text,text,date,bigint,text,boolean)
                 TO %I', desktop_login);
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN member_id;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_enroll_member(uuid, text, name, boolean, boolean) FROM PUBLIC;
