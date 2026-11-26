-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.has_project_role(tenant uuid, project uuid, roles text[])
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
                  AND grant_row.role = ANY(roles)
            )
            OR EXISTS (
                SELECT FROM vault.project_group_shares AS grant_row
                WHERE grant_row.organization_id = tenant AND grant_row.project_id = project
                  AND grant_row.role = ANY(roles)
            )
        )
    $$;

CREATE POLICY read_live_assignments ON vault.assignments FOR SELECT
    USING (
        NOT retired AND (
            vault.has_project_role(organization_id, project_id, ARRAY['manager'])
            OR EXISTS (
                SELECT FROM vault.resource_user_mappings AS mapping
                WHERE mapping.organization_id = assignments.organization_id
                  AND mapping.project_id = assignments.project_id
                  AND mapping.resource_id = assignments.resource_id
                  AND mapping.effective_from <= transaction_timestamp()
                  AND vault.has_project_role(assignments.organization_id, assignments.project_id,
                      ARRAY['contributor', 'editor', 'viewer', 'time_entry'])
            )
        )
    );

CREATE POLICY read_live_tasks ON vault.tasks FOR SELECT
    USING (
        NOT retired AND (
            vault.has_project_role(organization_id, project_id, ARRAY['manager', 'editor', 'viewer', 'time_entry'])
            OR EXISTS (
                SELECT FROM vault.assignments AS assignment
                WHERE assignment.organization_id = tasks.organization_id
                  AND assignment.project_id = tasks.project_id
                  AND assignment.task_id = tasks.id
            )
        )
    );

CREATE POLICY insert_own_draft_time ON vault.time_entries FOR INSERT
    WITH CHECK (
        organization_id = vault.context_id('app.current_organization_id')
        AND user_id = vault.context_id('app.current_user_id')
        AND status = 'draft'
        AND EXISTS (
            SELECT FROM vault.projects AS project
            WHERE project.organization_id = time_entries.organization_id
              AND project.id = time_entries.project_id AND NOT project.archived
        )
        AND EXISTS (
            SELECT FROM vault.tasks AS task
            WHERE task.organization_id = time_entries.organization_id
              AND task.project_id = time_entries.project_id AND task.id = time_entries.task_id
        )
        AND (
            vault.has_project_role(organization_id, project_id, ARRAY['time_entry'])
            OR EXISTS (
                SELECT FROM vault.assignments AS assignment
                JOIN vault.resource_user_mappings AS mapping
                  ON mapping.organization_id = assignment.organization_id
                 AND mapping.project_id = assignment.project_id
                 AND mapping.resource_id = assignment.resource_id
                WHERE assignment.organization_id = time_entries.organization_id
                  AND assignment.project_id = time_entries.project_id
                  AND assignment.task_id = time_entries.task_id
                  AND (time_entries.assignment_id IS NULL OR assignment.id = time_entries.assignment_id)
                  AND mapping.effective_from <= transaction_timestamp()
                  AND vault.has_project_role(time_entries.organization_id, time_entries.project_id,
                      ARRAY['contributor', 'editor', 'viewer', 'time_entry', 'manager'])
            )
        )
        AND (
            assignment_id IS NULL OR EXISTS (
                SELECT FROM vault.assignments AS assignment
                WHERE assignment.organization_id = time_entries.organization_id
                  AND assignment.project_id = time_entries.project_id
                  AND assignment.id = time_entries.assignment_id
                  AND assignment.task_id = time_entries.task_id
            )
        )
    );
