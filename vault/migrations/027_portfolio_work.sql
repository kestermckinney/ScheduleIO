-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.desktop_portfolio_work(tenant uuid, team uuid DEFAULT NULL)
    RETURNS TABLE (
        project_id uuid, project_title text, user_id uuid, planned_millis numeric,
        imported_actual_millis numeric, remaining_millis numeric, approved_millis numeric)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF team IS NOT NULL AND NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required for group reports'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        RETURN QUERY
            WITH authorized_projects AS (
                SELECT project.id, project.title
                FROM vault.projects AS project
                WHERE project.organization_id = tenant AND NOT project.archived
                  AND vault.has_project_role(tenant, project.id, ARRAY['manager'])
            ), report_users AS (
                SELECT member.user_id FROM vault.group_members AS member
                WHERE member.organization_id = tenant AND member.group_id = team
            ), planned AS (
                SELECT project.id, project.title, mapping.user_id,
                       sum((assignment.properties->>'work_millis')::numeric) AS planned,
                       sum((assignment.properties->>'actual_work_millis')::numeric) AS imported_actual,
                       sum((assignment.properties->>'remaining_work_millis')::numeric) AS remaining,
                       0::numeric AS approved
                FROM authorized_projects AS project
                JOIN vault.assignments AS assignment
                  ON assignment.organization_id = tenant AND assignment.project_id = project.id
                 AND NOT assignment.retired AND assignment.properties ? 'work_millis'
                JOIN vault.resources AS resource
                  ON resource.organization_id = tenant AND resource.project_id = project.id
                 AND resource.id = assignment.resource_id AND NOT resource.retired
                 AND resource.properties->>'kind' = 'work'
                LEFT JOIN vault.resource_user_mappings AS mapping
                  ON mapping.organization_id = tenant AND mapping.project_id = project.id
                 AND mapping.resource_id = resource.id AND mapping.effective_until IS NULL
                 AND mapping.effective_from <= transaction_timestamp()
                WHERE team IS NULL OR mapping.user_id IN (SELECT member.user_id FROM report_users AS member)
                GROUP BY project.id, project.title, mapping.user_id
            ), reported_time AS (
                SELECT project.id, project.title, entry.user_id, 0::numeric, 0::numeric,
                       0::numeric, sum(entry.work_millis)::numeric
                FROM authorized_projects AS project
                JOIN vault.time_entries AS entry ON entry.organization_id = tenant
                 AND entry.project_id = project.id AND entry.status = 'approved'
                WHERE team IS NULL OR entry.user_id IN (SELECT member.user_id FROM report_users AS member)
                GROUP BY project.id, project.title, entry.user_id
            )
            SELECT totals.id, totals.title, totals.user_id, sum(totals.planned),
                   sum(totals.imported_actual), sum(totals.remaining), sum(totals.approved)
            FROM (SELECT * FROM planned UNION ALL SELECT * FROM reported_time) AS totals
            GROUP BY totals.id, totals.title, totals.user_id
            ORDER BY totals.title, totals.id, totals.user_id NULLS LAST;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.desktop_portfolio_work(uuid, uuid) FROM PUBLIC;
