-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- Populate this private registry through privileged Vault provisioning. Match
-- both role OID and name so a dropped and recreated login needs a new binding.
CREATE TABLE vault.manager_principals (
    database_role_id oid PRIMARY KEY,
    database_role_name name NOT NULL UNIQUE,
    user_id uuid NOT NULL REFERENCES vault.users,
    active boolean NOT NULL DEFAULT true
);

ALTER TABLE vault.manager_principals ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.manager_principals FORCE ROW LEVEL SECURITY;

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY manage_manager_principals ON vault.manager_principals
         FOR ALL TO %I USING (true) WITH CHECK (true)', current_user);
END
$migration$;

CREATE FUNCTION vault.bind_manager_context(tenant uuid)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        actor uuid;
    BEGIN
        IF current_user IN (SELECT rolname FROM pg_roles WHERE rolsuper OR rolbypassrls) THEN
            RAISE EXCEPTION 'Desktop Vault operations require a migration owner subject to RLS'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        SELECT principal.user_id INTO actor
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login
          ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
        WHERE login.rolname = session_user AND login.rolcanlogin AND principal.active;

        IF actor IS NULL THEN
            RAISE EXCEPTION 'PostgreSQL login has no active Vault Manager identity'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        PERFORM set_config('app.current_organization_id', tenant::text, true);
        PERFORM set_config('app.current_user_id', actor::text, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.bind_manager_context(uuid) FROM PUBLIC;

CREATE FUNCTION vault.manager_import_initial_schedule(
    tenant uuid, new_project uuid, title text, source_namespace text, source_key text,
    content_digest text, metadata bytea, collections bytea,
    task_rows jsonb, resource_rows jsonb, assignment_rows jsonb)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        imported uuid;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        imported := vault.import_initial_schedule(
            tenant, new_project, title, source_namespace, source_key, content_digest,
            metadata, collections, task_rows, resource_rows, assignment_rows);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN imported;
    END
    $$;

CREATE FUNCTION vault.manager_lookup_import_source(
    tenant uuid, digest text, external_namespace text, external_key text)
    RETURNS TABLE (outcome text, project_id uuid)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        RETURN QUERY SELECT lookup.outcome, lookup.project_id
        FROM vault.lookup_import_source(tenant, digest, external_namespace, external_key) AS lookup;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_import_initial_schedule(
    uuid, uuid, text, text, text, text, bytea, bytea, jsonb, jsonb, jsonb) FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.manager_lookup_import_source(uuid, text, text, text) FROM PUBLIC;
