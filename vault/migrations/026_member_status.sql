-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
DECLARE
    membership_scope text := '
        organization_id = vault.context_id(''app.current_organization_id'')
        AND role = ''member'' AND user_id <> vault.context_id(''app.current_user_id'')';
BEGIN
    -- The schema owner uses these policies only after the operation checks
    -- the authenticated actor's own active administrator membership.
    EXECUTE format(
        'CREATE POLICY inspect_members_for_status ON vault.organization_members
         FOR SELECT TO %I USING (%s)', current_user, membership_scope);
    EXECUTE format(
        'CREATE POLICY change_member_status ON vault.organization_members
         FOR UPDATE TO %I USING (%s) WITH CHECK (%s)',
        current_user, membership_scope, membership_scope);
    EXECUTE format(
        'CREATE POLICY append_member_status_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation IN (''organization.member_deactivated'', ''organization.member_reactivated'')
         )', current_user);
END
$migration$;

CREATE FUNCTION vault.manager_set_member_active(tenant uuid, member_id uuid, new_active boolean)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        changed boolean;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        UPDATE vault.organization_members AS member SET active = new_active
        WHERE member.organization_id = tenant AND member.user_id = member_id
          AND member.role = 'member' AND member.active IS DISTINCT FROM new_active;
        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, vault.context_id('app.current_user_id'),
                    CASE WHEN new_active THEN 'organization.member_reactivated'
                                         ELSE 'organization.member_deactivated' END,
                    jsonb_build_object('user_id', member_id));
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_set_member_active(uuid, uuid, boolean) FROM PUBLIC;
