-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY change_project_lifecycle ON vault.projects FOR UPDATE TO %I
         USING (vault.is_organization_admin(organization_id))
         WITH CHECK (vault.is_organization_admin(organization_id))', current_user);
    EXECUTE format(
        'CREATE POLICY append_project_lifecycle_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation IN (''project.archived'', ''project.restored'')
         )', current_user);
END
$migration$;

-- Recheck lifecycle state on the row actually updated so a concurrent archive
-- cannot race a save that already checked its caller's authorization.
CREATE OR REPLACE FUNCTION vault.begin_schedule_update(tenant uuid, project uuid, expected_edit_counter bigint,
                                                       new_title text)
    RETURNS bigint
    LANGUAGE sql VOLATILE
    SET search_path = pg_catalog
    AS $$
        UPDATE vault.projects
           SET edit_counter = edit_counter + 1, title = new_title
         WHERE organization_id = tenant AND id = project AND NOT archived
           AND edit_counter = expected_edit_counter
         RETURNING edit_counter
    $$;

CREATE FUNCTION vault.manager_set_project_archived(tenant uuid, project uuid, archive_project boolean)
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

        UPDATE vault.projects AS item SET archived = archive_project
        WHERE item.organization_id = tenant AND item.id = project
          AND item.archived IS DISTINCT FROM archive_project;
        changed := FOUND;

        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, project_id, actor_id, operation)
            VALUES (tenant, project, vault.context_id('app.current_user_id'),
                    CASE WHEN archive_project THEN 'project.archived' ELSE 'project.restored' END);
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_set_project_archived(uuid, uuid, boolean) FROM PUBLIC;
