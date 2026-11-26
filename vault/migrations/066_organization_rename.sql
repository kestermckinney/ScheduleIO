-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY rename_organization ON vault.organizations FOR UPDATE TO %I
         USING (vault.is_organization_admin(id)) WITH CHECK (vault.is_organization_admin(id))', current_user);
    EXECUTE format(
        'CREATE POLICY append_organization_name_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''organization.renamed''
         )', current_user);
END
$migration$;

CREATE FUNCTION vault.manager_rename_organization(tenant uuid, new_name text)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        changed boolean;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Organization administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF new_name IS NULL OR char_length(btrim(new_name)) NOT BETWEEN 1 AND 200 THEN
            RAISE EXCEPTION 'Enter an organization name of 1 to 200 characters'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        UPDATE vault.organizations AS organization SET name = btrim(new_name)
        WHERE organization.id = tenant AND organization.name IS DISTINCT FROM btrim(new_name);
        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, vault.context_id('app.current_user_id'), 'organization.renamed',
                    jsonb_build_object('name', btrim(new_name)));
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;
