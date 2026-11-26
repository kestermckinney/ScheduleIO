-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE vault.projects (
    organization_id uuid NOT NULL REFERENCES vault.organizations,
    id uuid NOT NULL,
    title text NOT NULL,
    archived boolean NOT NULL DEFAULT false,
    edit_counter bigint NOT NULL DEFAULT 0 CHECK (edit_counter >= 0),
    PRIMARY KEY (organization_id, id)
);

CREATE TABLE vault.project_import_sources (
    organization_id uuid NOT NULL,
    source_namespace text NOT NULL CHECK (btrim(source_namespace) <> ''),
    source_key text NOT NULL CHECK (btrim(source_key) <> ''),
    project_id uuid NOT NULL,
    PRIMARY KEY (organization_id, source_namespace, source_key),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects
);

CREATE TABLE vault.project_members (
    organization_id uuid NOT NULL,
    project_id uuid NOT NULL,
    user_id uuid NOT NULL,
    role text NOT NULL CHECK (role IN ('manager', 'editor', 'viewer', 'contributor', 'time_entry', 'time_approver')),
    PRIMARY KEY (organization_id, project_id, user_id, role),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects,
    FOREIGN KEY (organization_id, user_id) REFERENCES vault.organization_members
);

CREATE TABLE vault.project_group_shares (
    organization_id uuid NOT NULL,
    project_id uuid NOT NULL,
    group_id uuid NOT NULL,
    role text NOT NULL CHECK (role IN ('manager', 'editor', 'viewer', 'contributor', 'time_entry', 'time_approver')),
    PRIMARY KEY (organization_id, project_id, group_id, role),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects,
    FOREIGN KEY (organization_id, group_id) REFERENCES vault.groups
);

CREATE INDEX project_import_sources_by_project
    ON vault.project_import_sources (organization_id, project_id);
CREATE INDEX project_members_by_user
    ON vault.project_members (organization_id, user_id, project_id);
CREATE INDEX project_group_shares_by_group
    ON vault.project_group_shares (organization_id, group_id, project_id);

ALTER TABLE vault.projects ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.projects FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.project_import_sources ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.project_import_sources FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.project_members ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.project_members FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.project_group_shares ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.project_group_shares FORCE ROW LEVEL SECURITY;

CREATE POLICY read_own_project_grants ON vault.project_members FOR SELECT
    USING (
        organization_id = vault.context_id('app.current_organization_id')
        AND user_id = vault.context_id('app.current_user_id')
        AND EXISTS (
            SELECT 1 FROM vault.organization_members AS member
            WHERE member.organization_id = project_members.organization_id
              AND member.user_id = project_members.user_id
        )
    );

CREATE POLICY read_joined_group_grants ON vault.project_group_shares FOR SELECT
    USING (
        organization_id = vault.context_id('app.current_organization_id')
        AND EXISTS (
            SELECT 1 FROM vault.group_members AS member
            WHERE member.organization_id = project_group_shares.organization_id
              AND member.group_id = project_group_shares.group_id
              AND member.user_id = vault.context_id('app.current_user_id')
        )
    );

CREATE POLICY read_accessible_projects ON vault.projects FOR SELECT
    USING (
        organization_id = vault.context_id('app.current_organization_id')
        AND (
            EXISTS (
                SELECT 1 FROM vault.organization_members AS member
                WHERE member.organization_id = projects.organization_id
                  AND member.user_id = vault.context_id('app.current_user_id')
                  AND member.role IN ('owner', 'admin')
            )
            OR EXISTS (
                SELECT 1 FROM vault.project_members AS grant_row
                WHERE grant_row.organization_id = projects.organization_id
                  AND grant_row.project_id = projects.id
            )
            OR EXISTS (
                SELECT 1 FROM vault.project_group_shares AS grant_row
                WHERE grant_row.organization_id = projects.organization_id
                  AND grant_row.project_id = projects.id
            )
        )
    );
