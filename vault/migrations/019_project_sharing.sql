-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
DECLARE
    relation text;
BEGIN
    FOREACH relation IN ARRAY ARRAY['project_members', 'project_group_shares']
    LOOP
        EXECUTE format(
            'CREATE POLICY manage_project_shares ON vault.%I FOR ALL TO %I
             USING (vault.is_organization_admin(organization_id))
             WITH CHECK (vault.is_organization_admin(organization_id))',
            relation, current_user);
    END LOOP;
    EXECUTE format(
        'CREATE POLICY append_project_share_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation IN (''project.access_granted'', ''project.access_revoked'')
         )', current_user);
END
$migration$;

CREATE FUNCTION vault.manager_change_project_share(
    tenant uuid, project uuid, principal_kind text, principal_id uuid, project_role text,
    grant_access boolean)
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
        IF principal_kind IS NULL OR principal_kind NOT IN ('user', 'group')
            OR principal_id IS NULL OR grant_access IS NULL OR project_role IS NULL
            OR project_role NOT IN ('manager', 'editor', 'viewer', 'contributor', 'time_entry', 'time_approver') THEN
            RAISE EXCEPTION 'Invalid project access parameters' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        IF principal_kind = 'user' AND grant_access THEN
            INSERT INTO vault.project_members (organization_id, project_id, user_id, role)
            VALUES (tenant, project, principal_id, project_role)
            ON CONFLICT DO NOTHING;
        ELSIF principal_kind = 'group' AND grant_access THEN
            INSERT INTO vault.project_group_shares (organization_id, project_id, group_id, role)
            VALUES (tenant, project, principal_id, project_role)
            ON CONFLICT DO NOTHING;
        ELSIF principal_kind = 'user' THEN
            DELETE FROM vault.project_members
            WHERE organization_id = tenant AND project_id = project
              AND user_id = principal_id AND role = project_role;
        ELSE
            DELETE FROM vault.project_group_shares
            WHERE organization_id = tenant AND project_id = project
              AND group_id = principal_id AND role = project_role;
        END IF;

        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, project_id, actor_id, operation, details)
            VALUES (tenant, project, vault.context_id('app.current_user_id'),
                    CASE WHEN grant_access THEN 'project.access_granted' ELSE 'project.access_revoked' END,
                    jsonb_build_object('principal_kind', principal_kind, 'principal_id', principal_id,
                                       'role', project_role));
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_change_project_share(uuid, uuid, text, uuid, text, boolean)
    FROM PUBLIC;
