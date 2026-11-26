-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE vault.transfer_jobs (
    organization_id uuid NOT NULL,
    id uuid NOT NULL,
    project_id uuid,
    actor_id uuid NOT NULL,
    request_id uuid NOT NULL,
    operation text NOT NULL CHECK (operation IN ('import', 'export')),
    status text NOT NULL CHECK (status IN ('queued', 'running', 'complete', 'failed', 'cancelled')),
    created_at timestamptz NOT NULL DEFAULT transaction_timestamp(),
    finished_at timestamptz,
    diagnostics jsonb NOT NULL DEFAULT '[]' CHECK (jsonb_typeof(diagnostics) = 'array'),
    PRIMARY KEY (organization_id, id),
    UNIQUE (organization_id, actor_id, request_id),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects,
    FOREIGN KEY (organization_id, actor_id) REFERENCES vault.organization_members,
    CHECK ((finished_at IS NOT NULL) = (status IN ('complete', 'failed', 'cancelled'))),
    CHECK (finished_at IS NULL OR finished_at >= created_at)
);

CREATE INDEX transfer_jobs_by_project
    ON vault.transfer_jobs (organization_id, project_id, created_at DESC);

CREATE TABLE vault.audit_events (
    organization_id uuid NOT NULL,
    id bigint GENERATED ALWAYS AS IDENTITY,
    project_id uuid,
    actor_id uuid NOT NULL,
    operation text NOT NULL CHECK (btrim(operation) <> ''),
    occurred_at timestamptz NOT NULL DEFAULT transaction_timestamp(),
    details jsonb NOT NULL DEFAULT '{}' CHECK (jsonb_typeof(details) = 'object'),
    PRIMARY KEY (organization_id, id),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects,
    FOREIGN KEY (organization_id, actor_id) REFERENCES vault.organization_members
);

CREATE INDEX audit_events_by_project
    ON vault.audit_events (organization_id, project_id, occurred_at DESC);
CREATE INDEX audit_events_by_actor
    ON vault.audit_events (organization_id, actor_id, occurred_at DESC);

ALTER TABLE vault.transfer_jobs ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.transfer_jobs FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.audit_events ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.audit_events FORCE ROW LEVEL SECURITY;

CREATE POLICY read_own_transfer_jobs ON vault.transfer_jobs FOR SELECT
    USING (
        actor_id = vault.context_id('app.current_user_id')
        AND EXISTS (
            SELECT FROM vault.organization_members AS member
            WHERE member.organization_id = transfer_jobs.organization_id
              AND member.user_id = transfer_jobs.actor_id
        )
        AND (
            project_id IS NULL OR EXISTS (
                SELECT FROM vault.projects AS project
                WHERE project.organization_id = transfer_jobs.organization_id
                  AND project.id = transfer_jobs.project_id
            )
        )
    );

CREATE POLICY read_admin_audit ON vault.audit_events FOR SELECT
    USING (
        vault.has_project_role(organization_id, project_id, ARRAY['manager'])
    );
