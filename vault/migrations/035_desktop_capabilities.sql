-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- A schema owner can grant named sets of entry points to a registered login.
-- Vault membership and project roles remain checked by those entry points.
CREATE FUNCTION vault.enable_desktop_capability(desktop_login name, capability text)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        routines text[];
        routine text;
    BEGIN
        IF NOT EXISTS (
            SELECT FROM pg_namespace AS schema
            JOIN pg_roles AS owner ON owner.oid = schema.nspowner
            WHERE schema.nspname = 'vault' AND owner.rolname = current_user
              AND NOT owner.rolsuper AND NOT owner.rolbypassrls
        ) THEN
            RAISE EXCEPTION 'Connect as the Vault schema owner'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF NOT EXISTS (
            SELECT FROM vault.manager_principals AS principal
            JOIN pg_roles AS login
              ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
            WHERE login.rolname = desktop_login AND login.rolcanlogin AND principal.active
              AND NOT login.rolsuper AND NOT login.rolbypassrls AND NOT login.rolcreaterole
              AND NOT login.rolcreatedb AND NOT login.rolreplication
              AND NOT pg_has_role(login.oid, current_user, 'MEMBER')
        ) THEN
            RAISE EXCEPTION 'Select an active, registered, unprivileged Vault login'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        CASE capability
            WHEN 'directory' THEN
                routines := ARRAY['vault.desktop_list_projects(uuid)'];
            WHEN 'management' THEN
                routines := ARRAY[
                    'vault.manager_enroll_member(uuid,text,name,boolean,boolean)',
                    'vault.manager_set_member_active(uuid,uuid,boolean)',
                    'vault.manager_change_group(uuid,uuid,text,text,uuid)',
                    'vault.manager_change_project_share(uuid,uuid,text,uuid,text,boolean)',
                    'vault.manager_set_project_archived(uuid,uuid,boolean)'];
            WHEN 'resources' THEN
                routines := ARRAY[
                    'vault.manager_list_resources(uuid,uuid)',
                    'vault.manager_set_resource_user(uuid,uuid,uuid,uuid)',
                    'vault.desktop_portfolio_work(uuid,uuid)'];
            WHEN 'audit' THEN
                routines := ARRAY[
                    'vault.manager_list_audit(uuid,uuid,integer)',
                    'vault.manager_check_project_access(uuid,uuid,uuid)'];
            WHEN 'transfers' THEN
                routines := ARRAY[
                    'vault.manager_record_transfer(uuid,uuid,uuid,text,boolean,text)',
                    'vault.manager_list_transfers(uuid)'];
            WHEN 'export' THEN
                routines := ARRAY[
                    'vault.desktop_read_schedule(uuid,uuid)',
                    'vault.desktop_approved_export_work(uuid,uuid)'];
            WHEN 'time' THEN
                routines := ARRAY[
                    'vault.desktop_list_time_work(uuid,uuid)',
                    'vault.desktop_list_time(uuid,uuid)',
                    'vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean)',
                    'vault.desktop_update_time(uuid,uuid,uuid,text,text,date,bigint,text,boolean)'];
            ELSE
                RAISE EXCEPTION 'Unknown Vault desktop capability' USING ERRCODE = 'invalid_parameter_value';
        END CASE;

        EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', desktop_login);
        FOREACH routine IN ARRAY routines LOOP
            EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO %I', routine, desktop_login);
        END LOOP;
    END
    $$;

REVOKE ALL ON FUNCTION vault.enable_desktop_capability(name, text) FROM PUBLIC;
