-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
DECLARE
    transfer_scope text := '
        organization_id = vault.context_id(''app.current_organization_id'')
        AND vault.context_id(''app.ownership_transfer_target'') IS NOT NULL
        AND user_id IN (vault.context_id(''app.current_user_id''),
                        vault.context_id(''app.ownership_transfer_target''))';
BEGIN
    EXECUTE format(
        'CREATE POLICY inspect_ownership_transfer ON vault.organization_members
         FOR SELECT TO %I USING (%s AND user_id <> vault.context_id(''app.current_user_id''))', current_user, transfer_scope);
    EXECUTE format(
        'CREATE POLICY change_ownership_transfer ON vault.organization_members
         FOR UPDATE TO %I USING (%s) WITH CHECK (%s)', current_user, transfer_scope, transfer_scope);
    EXECUTE format(
        'CREATE POLICY append_ownership_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''organization.ownership_transferred''
         )', current_user);
END
$migration$;

CREATE FUNCTION vault.manager_transfer_ownership(tenant uuid, new_owner uuid)
    RETURNS void
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        changed integer;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM set_config('app.ownership_transfer_target', '', true);
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        PERFORM pg_advisory_xact_lock(hashtextextended(tenant::text, 38));
        IF NOT EXISTS (
            SELECT FROM vault.organization_members AS member
            WHERE member.organization_id = tenant AND member.user_id = actor
              AND member.role = 'owner' AND member.active
        ) THEN
            RAISE EXCEPTION 'Organization owner required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        IF new_owner IS NULL OR new_owner = actor OR NOT EXISTS (
            SELECT FROM vault.manager_principals AS principal
            JOIN pg_roles AS login
              ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
            WHERE principal.user_id = new_owner AND principal.active AND login.rolcanlogin
        ) THEN
            RAISE EXCEPTION 'Select a different member with an active Vault login'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM set_config('app.current_user_id', new_owner::text, true);
        IF NOT EXISTS (SELECT FROM vault.users AS identity WHERE identity.id = new_owner AND identity.active) THEN
            RAISE EXCEPTION 'Select an active Vault user' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM set_config('app.current_user_id', actor::text, true);
        PERFORM set_config('app.ownership_transfer_target', new_owner::text, true);

        UPDATE vault.organization_members AS member
        SET role = CASE WHEN member.user_id = actor THEN 'admin' ELSE 'owner' END
        WHERE member.organization_id = tenant AND member.active
          AND ((member.user_id = actor AND member.role = 'owner')
               OR (member.user_id = new_owner AND member.role IN ('member', 'admin')));
        GET DIAGNOSTICS changed = ROW_COUNT;
        IF changed <> 2 THEN
            RAISE EXCEPTION 'Ownership requires an active member in this organization'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, actor, 'organization.ownership_transferred',
                jsonb_build_object('new_owner', new_owner));
        PERFORM set_config('app.ownership_transfer_target', '', true);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
