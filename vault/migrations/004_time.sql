-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE vault.resource_user_mappings (
    organization_id uuid NOT NULL,
    project_id uuid NOT NULL,
    id uuid NOT NULL,
    resource_id uuid NOT NULL,
    user_id uuid NOT NULL,
    effective_from timestamptz NOT NULL DEFAULT transaction_timestamp(),
    effective_until timestamptz,
    PRIMARY KEY (organization_id, project_id, id),
    FOREIGN KEY (organization_id, project_id, resource_id) REFERENCES vault.resources,
    FOREIGN KEY (organization_id, user_id) REFERENCES vault.organization_members,
    CHECK (effective_until IS NULL OR effective_until > effective_from)
);

CREATE UNIQUE INDEX active_mapping_by_resource
    ON vault.resource_user_mappings (organization_id, project_id, resource_id)
    WHERE effective_until IS NULL;

CREATE INDEX resource_mappings_by_user
    ON vault.resource_user_mappings (organization_id, user_id, project_id);

CREATE TABLE vault.time_entries (
    organization_id uuid NOT NULL,
    project_id uuid NOT NULL,
    id uuid NOT NULL,
    user_id uuid NOT NULL,
    task_id uuid NOT NULL,
    assignment_id uuid,
    work_date date NOT NULL,
    work_millis bigint NOT NULL CHECK (work_millis > 0),
    comments text NOT NULL DEFAULT '',
    billable boolean NOT NULL DEFAULT false,
    status text NOT NULL DEFAULT 'draft'
        CHECK (status IN ('draft', 'submitted', 'approved', 'returned')),
    PRIMARY KEY (organization_id, project_id, id),
    FOREIGN KEY (organization_id, user_id) REFERENCES vault.organization_members,
    FOREIGN KEY (organization_id, project_id, task_id) REFERENCES vault.tasks,
    FOREIGN KEY (organization_id, project_id, assignment_id, task_id)
        REFERENCES vault.assignments (organization_id, project_id, id, task_id)
);

CREATE INDEX time_entries_by_logger
    ON vault.time_entries (organization_id, user_id, work_date);
CREATE INDEX time_entries_by_review_status
    ON vault.time_entries (organization_id, project_id, status, work_date);
CREATE INDEX time_entries_by_task
    ON vault.time_entries (organization_id, project_id, task_id);
CREATE INDEX time_entries_by_assignment
    ON vault.time_entries (organization_id, project_id, assignment_id, task_id)
    WHERE assignment_id IS NOT NULL;

CREATE TABLE vault.time_entry_events (
    organization_id uuid NOT NULL,
    project_id uuid NOT NULL,
    entry_id uuid NOT NULL,
    id bigint GENERATED ALWAYS AS IDENTITY,
    actor_id uuid NOT NULL,
    event_type text NOT NULL CHECK (event_type IN ('created', 'submitted', 'approved', 'returned')),
    occurred_at timestamptz NOT NULL DEFAULT transaction_timestamp(),
    PRIMARY KEY (organization_id, project_id, entry_id, id),
    FOREIGN KEY (organization_id, project_id, entry_id) REFERENCES vault.time_entries,
    FOREIGN KEY (organization_id, actor_id) REFERENCES vault.organization_members
);

ALTER TABLE vault.resource_user_mappings ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.resource_user_mappings FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.time_entries ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.time_entries FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.time_entry_events ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.time_entry_events FORCE ROW LEVEL SECURITY;

CREATE POLICY read_own_active_mappings ON vault.resource_user_mappings FOR SELECT
    USING (
        effective_until IS NULL
        AND user_id = vault.context_id('app.current_user_id')
        AND EXISTS (
            SELECT 1 FROM vault.projects AS project
            WHERE project.organization_id = resource_user_mappings.organization_id
              AND project.id = resource_user_mappings.project_id
        )
    );

CREATE FUNCTION vault.can_read_time_entry(tenant uuid, project uuid, logger uuid)
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT
        tenant = vault.context_id('app.current_organization_id')
        AND EXISTS (
            SELECT 1 FROM vault.organization_members AS member
            WHERE member.organization_id = tenant
              AND member.user_id = vault.context_id('app.current_user_id')
              AND (
                  member.user_id = logger
                  OR member.role IN ('owner', 'admin')
                  OR EXISTS (
                      SELECT 1 FROM vault.project_members AS grant_row
                      WHERE grant_row.organization_id = tenant
                        AND grant_row.project_id = project
                        AND grant_row.user_id = member.user_id
                        AND grant_row.role IN ('manager', 'time_approver')
                  )
                  OR EXISTS (
                      SELECT 1 FROM vault.project_group_shares AS grant_row
                      WHERE grant_row.organization_id = tenant
                        AND grant_row.project_id = project
                        AND grant_row.role IN ('manager', 'time_approver')
                  )
              )
        )
    $$;

CREATE POLICY read_time_entries ON vault.time_entries FOR SELECT
    USING (vault.can_read_time_entry(organization_id, project_id, user_id));

CREATE POLICY read_visible_time_entry_events ON vault.time_entry_events FOR SELECT
    USING (
        EXISTS (
            SELECT 1 FROM vault.time_entries AS entry
            WHERE entry.organization_id = time_entry_events.organization_id
              AND entry.project_id = time_entry_events.project_id
              AND entry.id = time_entry_events.entry_id
        )
    );
