-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.desktop_approved_export_work(tenant uuid, project uuid)
    RETURNS TABLE (
        assignment_uid integer, period_start timestamptz, period_end timestamptz,
        work_millis numeric, entry_count bigint, work_date date)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.has_project_role(tenant, project, ARRAY['manager']), false) THEN
            RAISE EXCEPTION 'Full schedule access required for actual work export'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        RETURN QUERY
        SELECT assignment.source_uid,
               entry.work_date::timestamp AT TIME ZONE organization.time_zone,
               (entry.work_date + 1)::timestamp AT TIME ZONE organization.time_zone,
               sum(entry.work_millis), count(*), entry.work_date
        FROM vault.time_entries AS entry
        JOIN vault.organizations AS organization ON organization.id = entry.organization_id
        LEFT JOIN vault.assignments AS assignment
          ON (assignment.organization_id, assignment.project_id, assignment.id)
             = (entry.organization_id, entry.project_id, entry.assignment_id) AND NOT assignment.retired
        WHERE entry.organization_id = tenant AND entry.project_id = project AND entry.status = 'approved'
        GROUP BY assignment.source_uid, entry.work_date, organization.time_zone
        ORDER BY assignment.source_uid NULLS LAST, entry.work_date;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.desktop_approved_export_work(uuid, uuid) FROM PUBLIC;
