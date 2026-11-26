-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
DECLARE
    relation text;
BEGIN
    FOREACH relation IN ARRAY ARRAY['organizations', 'users', 'organization_members']
    LOOP
        EXECUTE format(
            'CREATE POLICY provision_identity ON vault.%I FOR INSERT TO %I WITH CHECK (true)',
            relation, current_user);
    END LOOP;
END
$migration$;

-- Run only as the non-superuser schema owner. Login creation and password
-- management stay with PostgreSQL. New Vault IDs are chosen by the caller.
CREATE FUNCTION vault.provision_organization(
    tenant uuid, organization_name text, organization_time_zone text,
    actor uuid, display_name text, desktop_login name,
    allow_initial_import boolean, allow_schedule_updates boolean)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        login_id oid;
    BEGIN
        IF EXISTS (SELECT FROM pg_roles WHERE rolname = current_user AND (rolsuper OR rolbypassrls)) THEN
            RAISE EXCEPTION 'Provision using the Vault schema owner subject to RLS'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
        SELECT oid INTO login_id FROM pg_roles
        WHERE rolname = desktop_login AND rolcanlogin AND NOT rolsuper
          AND NOT rolbypassrls AND NOT rolcreaterole AND NOT rolcreatedb
          AND NOT rolreplication AND NOT pg_has_role(oid, current_user, 'MEMBER');

        IF login_id IS NULL THEN
            RAISE EXCEPTION 'A separate unprivileged PostgreSQL login is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF NOT EXISTS (SELECT FROM pg_timezone_names WHERE name = organization_time_zone) THEN
            RAISE EXCEPTION 'Unknown organization time zone' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF NOT (coalesce(allow_initial_import, false) OR coalesce(allow_schedule_updates, false)) THEN
            RAISE EXCEPTION 'Select a desktop capability' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        INSERT INTO vault.organizations (id, name, time_zone)
        VALUES (tenant, organization_name, organization_time_zone);
        INSERT INTO vault.users (id, issuer, subject, display_name)
        VALUES (actor, 'postgresql', login_id::text, display_name);
        INSERT INTO vault.organization_members (organization_id, user_id, role)
        VALUES (tenant, actor, 'owner');
        INSERT INTO vault.manager_principals (database_role_id, database_role_name, user_id)
        VALUES (login_id, desktop_login, actor);

        EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', desktop_login);
        EXECUTE format(
            'GRANT EXECUTE ON FUNCTION vault.manager_lookup_import_source(uuid,text,text,text) TO %I',
            desktop_login);
        IF allow_initial_import THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.manager_import_initial_schedule(
                    uuid,uuid,text,text,text,text,bytea,bytea,jsonb,jsonb,jsonb) TO %I', desktop_login);
        END IF;
        IF allow_schedule_updates THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.desktop_read_schedule(uuid,uuid),
                    vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb),
                    vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)
                 TO %I', desktop_login);
        END IF;
    END
    $$;

REVOKE ALL ON FUNCTION vault.provision_organization(uuid,text,text,uuid,text,name,boolean,boolean)
    FROM PUBLIC;
