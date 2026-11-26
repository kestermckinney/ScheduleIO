-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY read_own_organization_directory ON vault.organization_members FOR SELECT TO %I
         USING (
             current_setting(''app.current_organization_id'', true) = ''''
             AND active AND user_id = vault.context_id(''app.current_user_id'')
             AND EXISTS (SELECT FROM vault.users WHERE id = user_id)
             AND EXISTS (
                 SELECT FROM vault.manager_principals AS principal JOIN pg_catalog.pg_roles AS login
                   ON (login.oid, login.rolname) = (principal.database_role_id, principal.database_role_name)
                 WHERE principal.user_id = organization_members.user_id AND principal.active
                   AND login.rolname = session_user AND login.rolcanlogin
             )
         )', current_user);
END
$migration$;

CREATE INDEX organization_members_active_by_user
    ON vault.organization_members (user_id, organization_id) WHERE active;

CREATE FUNCTION vault.manager_list_organizations()
    RETURNS TABLE (organization_id uuid, organization_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        tenant uuid;
    BEGIN
        -- Resolve the authenticated login and enter directory scope. Each name
        -- is then read under the ordinary single-organization policy.
        PERFORM vault.bind_manager_context(NULL);
        PERFORM set_config('app.current_organization_id', '', true);
        FOR tenant IN SELECT member.organization_id FROM vault.organization_members AS member
                      WHERE member.user_id = vault.context_id('app.current_user_id') AND member.active
                      ORDER BY member.organization_id
        LOOP
            PERFORM set_config('app.current_organization_id', tenant::text, true);
            RETURN QUERY SELECT organization.id, organization.name
                FROM vault.organizations AS organization WHERE organization.id = tenant;
        END LOOP;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
