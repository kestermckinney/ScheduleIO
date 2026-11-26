-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.manager_list_audit(tenant uuid, selected_project uuid, requested_limit integer)
    RETURNS TABLE (occurred_at timestamptz, project_id uuid, actor_id uuid, operation text, details jsonb)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.has_project_role(tenant, selected_project, ARRAY['manager']), false) THEN
            RAISE EXCEPTION 'Vault audit access requires an organization admin or project manager'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        RETURN QUERY
            SELECT note.event_at, note.project, note.actor, note.action, note.payload
            FROM (
                SELECT audit.occurred_at AS event_at, audit.project_id AS project,
                       audit.actor_id AS actor, audit.operation AS action, audit.details AS payload,
                       0 AS source, audit.id AS seq
                FROM vault.audit_events AS audit
                WHERE audit.organization_id = tenant
                  AND (selected_project IS NULL OR audit.project_id = selected_project)
                UNION ALL
                SELECT event.occurred_at, event.project_id, event.actor_id,
                       'time.' || event.event_type, jsonb_build_object('entry_id', event.entry_id), 1, event.id
                FROM vault.time_entry_events AS event
                WHERE event.organization_id = tenant
                  AND (selected_project IS NULL OR event.project_id = selected_project)
                  AND vault.has_project_role(tenant, event.project_id, ARRAY['manager'])
            ) AS note
            ORDER BY note.event_at DESC, note.source, note.seq DESC
            LIMIT least(200, greatest(1, coalesce(requested_limit, 100)));

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_list_audit(uuid, uuid, integer) FROM PUBLIC;
