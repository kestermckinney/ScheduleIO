-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.manager_export_security(tenant uuid) RETURNS jsonb
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        result jsonb;
        members jsonb := '[]'::jsonb;
        actor uuid;
        member_row record;
        identity_row record;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        IF NOT EXISTS (
            SELECT FROM vault.organization_members AS member
            WHERE member.organization_id = tenant
              AND member.user_id = vault.context_id('app.current_user_id')
              AND member.role = 'owner' AND member.active
        ) THEN
            RAISE EXCEPTION 'Organization owner required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        FOR member_row IN
            SELECT member.user_id, member.role, member.active
            FROM vault.organization_members AS member
            WHERE member.organization_id = tenant
            ORDER BY member.user_id
        LOOP
            -- Resolve only identities that belong to the selected organization.
            PERFORM set_config('app.current_user_id', member_row.user_id::text, true);
            SELECT identity.issuer,
                   CASE WHEN identity.issuer = 'postgresql' THEN login.rolname::text
                        ELSE identity.subject END AS subject
            INTO identity_row
            FROM vault.users AS identity
            LEFT JOIN vault.manager_principals AS principal
              ON identity.issuer = 'postgresql' AND principal.user_id = identity.id
             AND principal.database_role_id::text = identity.subject
            LEFT JOIN pg_roles AS login
              ON (login.oid, login.rolname) = (principal.database_role_id, principal.database_role_name)
            WHERE identity.id = member_row.user_id;
            members := members || jsonb_build_array(jsonb_build_object(
                'user_id', member_row.user_id, 'issuer', identity_row.issuer, 'subject', identity_row.subject,
                'role', member_row.role, 'active', member_row.active AND identity_row.issuer IS NOT NULL
            ));
        END LOOP;
        PERFORM set_config('app.current_user_id', actor::text, true);

        SELECT jsonb_build_object(
            'format', 1,
            'organization_id', tenant,
            'members', members,
            'groups', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'group_id', team.id, 'name', team.name, 'members', coalesce((
                        SELECT jsonb_agg(member.user_id ORDER BY member.user_id)
                        FROM vault.group_members AS member
                        WHERE (member.organization_id, member.group_id) = (tenant, team.id)
                    ), '[]'::jsonb)
                ) ORDER BY team.name, team.id)
                FROM vault.groups AS team WHERE team.organization_id = tenant
            ), '[]'::jsonb),
            'projects', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'project_id', project.id, 'title', project.title,
                    'user_grants', coalesce((
                        SELECT jsonb_agg(jsonb_build_object('user_id', grant_row.user_id, 'role', grant_row.role)
                                         ORDER BY grant_row.user_id, grant_row.role)
                        FROM vault.project_members AS grant_row
                        WHERE (grant_row.organization_id, grant_row.project_id) = (tenant, project.id)
                    ), '[]'::jsonb),
                    'group_grants', coalesce((
                        SELECT jsonb_agg(jsonb_build_object('group_id', grant_row.group_id, 'role', grant_row.role)
                                         ORDER BY grant_row.group_id, grant_row.role)
                        FROM vault.project_group_shares AS grant_row
                        WHERE (grant_row.organization_id, grant_row.project_id) = (tenant, project.id)
                    ), '[]'::jsonb)
                ) ORDER BY project.title, project.id)
                FROM vault.projects AS project WHERE project.organization_id = tenant
            ), '[]'::jsonb)
        ) INTO result;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN result;
    END
    $$;
