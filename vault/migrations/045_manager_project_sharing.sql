-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE OR REPLACE FUNCTION vault.has_project_role(tenant uuid, project uuid, roles text[])
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT tenant = vault.context_id('app.current_organization_id') AND (
            EXISTS (
                SELECT FROM vault.organization_members AS member
                WHERE member.organization_id = tenant
                  AND member.user_id = vault.context_id('app.current_user_id')
                  AND member.role IN ('owner', 'admin')
            )
            OR EXISTS (
                SELECT FROM vault.project_members AS grant_row
                WHERE grant_row.organization_id = tenant AND grant_row.project_id = project
                  AND grant_row.user_id = vault.context_id('app.current_user_id')
                  AND grant_row.role = ANY(roles)
            )
            OR EXISTS (
                SELECT FROM vault.project_group_shares AS grant_row
                WHERE grant_row.organization_id = tenant AND grant_row.project_id = project
                  AND EXISTS (
                      SELECT FROM vault.group_members AS member
                      WHERE (member.organization_id, member.group_id) = (grant_row.organization_id, grant_row.group_id)
                        AND member.user_id = vault.context_id('app.current_user_id')
                  )
                  AND grant_row.role = ANY(roles)
            )
        )
    $$;

-- During the role lookup, only ordinary grant visibility is needed. The
-- recursion guard can suppress elevated access, but never authorize it.
CREATE FUNCTION vault.can_manage_project_sharing(tenant uuid, project uuid)
    RETURNS boolean
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_guard text := coalesce(current_setting('app.checking_project_share_manager', true), '');
        permitted boolean;
    BEGIN
        IF vault.is_organization_admin(tenant) THEN
            RETURN true;
        END IF;
        IF previous_guard = 'on' THEN
            RETURN false;
        END IF;
        PERFORM set_config('app.checking_project_share_manager', 'on', true);
        permitted := vault.has_project_role(tenant, project, ARRAY['manager']);
        PERFORM set_config('app.checking_project_share_manager', previous_guard, true);
        RETURN coalesce(permitted, false);
    END
    $$;

ALTER POLICY manage_project_shares ON vault.project_members
    USING (vault.can_manage_project_sharing(organization_id, project_id))
    WITH CHECK (vault.can_manage_project_sharing(organization_id, project_id));

ALTER POLICY manage_project_shares ON vault.project_group_shares
    USING (vault.can_manage_project_sharing(organization_id, project_id))
    WITH CHECK (vault.can_manage_project_sharing(organization_id, project_id));

CREATE OR REPLACE FUNCTION vault.manager_change_project_share(
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
        IF NOT coalesce(vault.can_manage_project_sharing(tenant, project), false) THEN
            RAISE EXCEPTION 'Project manager or organization administrator required' USING ERRCODE = 'insufficient_privilege';
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
