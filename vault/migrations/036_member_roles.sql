-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
DECLARE
    membership_scope text := '
        organization_id = vault.context_id(''app.current_organization_id'')
        AND role IN (''member'', ''admin'')
        AND user_id <> vault.context_id(''app.current_user_id'')';
BEGIN
    EXECUTE format(
        'ALTER POLICY inspect_members_for_status ON vault.organization_members
         TO %I USING (%s)', current_user, membership_scope);
    EXECUTE format(
        'ALTER POLICY change_member_status ON vault.organization_members
         TO %I USING (%s) WITH CHECK (%s)', current_user, membership_scope, membership_scope);
    EXECUTE format(
        'CREATE POLICY append_member_role_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''organization.member_role_changed''
         )', current_user);
END
$migration$;

CREATE FUNCTION vault.manager_set_member_role(tenant uuid, member_id uuid, new_role text)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        changed boolean;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        IF NOT EXISTS (
            SELECT FROM vault.organization_members AS membership
            WHERE membership.organization_id = tenant AND membership.user_id = actor
              AND membership.role = 'owner'
        ) THEN
            RAISE EXCEPTION 'Organization owner required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF new_role IS NULL OR new_role NOT IN ('member', 'admin') THEN
            RAISE EXCEPTION 'Select the member or administrator role' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        UPDATE vault.organization_members AS membership SET role = new_role
        WHERE membership.organization_id = tenant AND membership.user_id = member_id
          AND membership.role IN ('member', 'admin') AND membership.role <> new_role
          AND membership.active;
        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, actor, 'organization.member_role_changed',
                    jsonb_build_object('user_id', member_id, 'role', new_role));
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;
