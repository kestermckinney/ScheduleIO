-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

ALTER POLICY append_group_audit ON vault.audit_events
    WITH CHECK (
        organization_id = vault.context_id('app.current_organization_id')
        AND actor_id = vault.context_id('app.current_user_id')
        AND operation IN ('group.created', 'group.member_added', 'group.member_removed', 'group.renamed')
    );

CREATE OR REPLACE FUNCTION vault.manager_change_group(
    tenant uuid, group_id_value uuid, operation_value text, group_name text, member_id uuid)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        event text;
        changed boolean;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        CASE operation_value
            WHEN 'create' THEN
                INSERT INTO vault.groups (organization_id, id, name)
                VALUES (tenant, group_id_value, group_name);
                event := 'group.created';
            WHEN 'rename' THEN
                IF group_name IS NULL OR btrim(group_name) = '' THEN
                    RAISE EXCEPTION 'Enter a group name' USING ERRCODE = 'invalid_parameter_value';
                END IF;
                UPDATE vault.groups AS team SET name = btrim(group_name)
                WHERE team.organization_id = tenant AND team.id = group_id_value
                  AND team.name IS DISTINCT FROM btrim(group_name);
                event := 'group.renamed';
            WHEN 'add_member' THEN
                INSERT INTO vault.group_members (organization_id, group_id, user_id)
                VALUES (tenant, group_id_value, member_id)
                ON CONFLICT DO NOTHING;
                event := 'group.member_added';
            WHEN 'remove_member' THEN
                DELETE FROM vault.group_members
                WHERE organization_id = tenant AND group_id = group_id_value AND user_id = member_id;
                event := 'group.member_removed';
            ELSE
                RAISE EXCEPTION 'Unsupported group operation' USING ERRCODE = 'invalid_parameter_value';
        END CASE;

        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, vault.context_id('app.current_user_id'), event,
                    jsonb_strip_nulls(jsonb_build_object('group_id', group_id_value, 'member_id', member_id,
                        'name', CASE WHEN operation_value = 'rename' THEN btrim(group_name) END)));
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;
