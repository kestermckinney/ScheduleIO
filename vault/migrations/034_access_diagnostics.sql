-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY append_access_diagnostic_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''security.project_access_checked''
         )', current_user);
END
$migration$;

CREATE FUNCTION vault.manager_check_project_access(tenant uuid, project uuid, target_user uuid)
    RETURNS TABLE (project_visible boolean, full_schedule_visible boolean, schedule_writable boolean,
                   can_review_time boolean)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        operator_id uuid;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF target_user IS NULL OR NOT EXISTS (
            SELECT FROM vault.projects AS item WHERE item.organization_id = tenant AND item.id = project
        ) THEN
            RAISE EXCEPTION 'Select a project in this organization and a user'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        operator_id := vault.context_id('app.current_user_id');

        PERFORM set_config('app.current_user_id', target_user::text, true);
        SELECT EXISTS (
            SELECT FROM vault.projects AS item WHERE item.organization_id = tenant AND item.id = project
        ), EXISTS (
            SELECT FROM vault.schedule_data AS schedule
            WHERE schedule.organization_id = tenant AND schedule.project_id = project
        ), coalesce(vault.can_write_schedule(tenant, project), false),
           coalesce(vault.has_project_role(tenant, project, ARRAY['manager', 'time_approver']), false)
        INTO project_visible, full_schedule_visible, schedule_writable, can_review_time;

        PERFORM set_config('app.current_user_id', operator_id::text, true);
        INSERT INTO vault.audit_events (organization_id, project_id, actor_id, operation, details)
        VALUES (tenant, project, operator_id, 'security.project_access_checked',
                jsonb_build_object('target_user', target_user));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN NEXT;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_check_project_access(uuid, uuid, uuid) FROM PUBLIC;
