-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- The Vault schema: every table, its indexes, and row-level security.
-- Later files add the functions, triggers, policies, and grants, by area.

DO $migration$
BEGIN
    IF current_setting('server_version_num')::integer < 160000 THEN
        RAISE EXCEPTION 'Schedule Vault requires PostgreSQL 16 or later'
            USING ERRCODE = 'feature_not_supported';
    END IF;
END
$migration$;

CREATE SCHEMA vault;

CREATE SCHEMA IF NOT EXISTS vault_admin;

-- Context is set transaction-locally by the trusted service. Missing, reset,
-- or malformed values cannot match a UUID.
CREATE FUNCTION vault.context_id(setting_name text) RETURNS uuid
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT CASE WHEN value ~* '^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$'
                    THEN value::uuid END
        FROM (SELECT current_setting(setting_name, true) AS value) AS context
    $$;

-- ── Organizations and people ──────────────────────────────────────────────

CREATE TABLE vault.organizations (
    id uuid PRIMARY KEY,
    name text NOT NULL CHECK (btrim(name) <> ''),
    time_zone text NOT NULL DEFAULT 'UTC',
    status text NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'suspended')),
    created_at timestamptz NOT NULL DEFAULT transaction_timestamp()
);

-- Only privileged identity management may access global identities directly.
-- A person's home organization is NULL only for platform operators.
-- sign_in_email is the address a person's identity-provider account carries,
-- used only to match that account with their Vault login.
CREATE TABLE vault.users (
    id uuid PRIMARY KEY,
    issuer text NOT NULL CHECK (btrim(issuer) <> ''),
    subject text NOT NULL CHECK (btrim(subject) <> ''),
    display_name text NOT NULL,
    active boolean NOT NULL DEFAULT true,
    home_organization_id uuid REFERENCES vault.organizations,
    sign_in_email text
        CHECK (sign_in_email IS NULL OR sign_in_email ~ '^[^@[:space:]]+@[^@[:space:]]+$'),
    UNIQUE (issuer, subject),
    CONSTRAINT users_home_identity UNIQUE (home_organization_id, id)
);

-- A membership must be in the member's home organization, so nobody can be
-- added to a second organization.
CREATE TABLE vault.organization_members (
    organization_id uuid NOT NULL REFERENCES vault.organizations,
    user_id uuid NOT NULL REFERENCES vault.users,
    role text NOT NULL CHECK (role IN ('owner', 'admin', 'member')),
    active boolean NOT NULL DEFAULT true,
    display_name text
        CHECK (display_name IS NULL OR (display_name = btrim(display_name) AND char_length(display_name) BETWEEN 1 AND 200)),
    PRIMARY KEY (organization_id, user_id),
    CONSTRAINT organization_members_home_organization
        FOREIGN KEY (organization_id, user_id) REFERENCES vault.users (home_organization_id, id)
);

CREATE INDEX organization_members_active_by_user
    ON vault.organization_members (user_id, organization_id) WHERE active;

CREATE TABLE vault.groups (
    organization_id uuid NOT NULL REFERENCES vault.organizations,
    id uuid NOT NULL,
    name text NOT NULL CHECK (btrim(name) <> ''),
    PRIMARY KEY (organization_id, id),
    UNIQUE (organization_id, name)
);

CREATE TABLE vault.group_members (
    organization_id uuid NOT NULL,
    group_id uuid NOT NULL,
    user_id uuid NOT NULL,
    PRIMARY KEY (organization_id, group_id, user_id),
    FOREIGN KEY (organization_id, group_id) REFERENCES vault.groups ON DELETE CASCADE,
    FOREIGN KEY (organization_id, user_id) REFERENCES vault.organization_members
);

CREATE INDEX group_members_by_user
    ON vault.group_members (organization_id, user_id, group_id);

-- Populate this private registry through privileged Vault provisioning. Match
-- both role OID and name so a dropped and recreated login needs a new binding.
CREATE TABLE vault.manager_principals (
    database_role_id oid PRIMARY KEY,
    database_role_name name NOT NULL UNIQUE,
    user_id uuid NOT NULL REFERENCES vault.users,
    active boolean NOT NULL DEFAULT true
);

-- ── Projects and sharing ──────────────────────────────────────────────────

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

CREATE INDEX project_import_sources_by_project
    ON vault.project_import_sources (organization_id, project_id);

-- Project access is either Edit ('editor') or View ('viewer').
--   View: open the schedule and see it, and enter one's own time on tasks
--         one is assigned to (through a resource mapped to them).
--   Edit: everything View allows, plus saving the schedule, sharing it,
--         entering time on any task for any member, and approving time.
-- Organization owners and administrators keep Edit access to every project.
CREATE TABLE vault.project_members (
    organization_id uuid NOT NULL,
    project_id uuid NOT NULL,
    user_id uuid NOT NULL,
    role text NOT NULL CONSTRAINT project_members_role_check CHECK (role IN ('editor', 'viewer')),
    PRIMARY KEY (organization_id, project_id, user_id, role),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects,
    FOREIGN KEY (organization_id, user_id) REFERENCES vault.organization_members
);

CREATE INDEX project_members_by_user
    ON vault.project_members (organization_id, user_id, project_id);

CREATE TABLE vault.project_group_shares (
    organization_id uuid NOT NULL,
    project_id uuid NOT NULL,
    group_id uuid NOT NULL,
    role text NOT NULL CONSTRAINT project_group_shares_role_check CHECK (role IN ('editor', 'viewer')),
    PRIMARY KEY (organization_id, project_id, group_id, role),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects,
    FOREIGN KEY (organization_id, group_id) REFERENCES vault.groups
);

CREATE INDEX project_group_shares_by_group
    ON vault.project_group_shares (organization_id, group_id, project_id);

-- ── Schedules ─────────────────────────────────────────────────────────────

CREATE TABLE vault.schedule_data (
    organization_id uuid NOT NULL,
    project_id uuid NOT NULL,
    format_revision integer NOT NULL CHECK (format_revision > 0),
    project_metadata bytea NOT NULL CHECK (octet_length(project_metadata) > 0),
    project_collections bytea NOT NULL CHECK (octet_length(project_collections) > 0),
    properties jsonb NOT NULL CHECK (jsonb_typeof(properties) = 'object'),
    PRIMARY KEY (organization_id, project_id),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects
);

CREATE TABLE vault.tasks (
    organization_id uuid NOT NULL,
    project_id uuid NOT NULL,
    id uuid NOT NULL,
    parent_id uuid,
    source_uid integer,
    position integer,
    properties jsonb,
    model_data bytea,
    retired boolean NOT NULL DEFAULT false,
    PRIMARY KEY (organization_id, project_id, id),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects,
    FOREIGN KEY (organization_id, project_id, parent_id) REFERENCES vault.tasks
        DEFERRABLE INITIALLY DEFERRED,
    CHECK (parent_id IS DISTINCT FROM id),
    CHECK (retired OR (position IS NOT NULL AND position >= 0
                       AND properties IS NOT NULL AND jsonb_typeof(properties) = 'object'
                       AND model_data IS NOT NULL AND octet_length(model_data) > 0))
);

CREATE UNIQUE INDEX live_task_uid
    ON vault.tasks (organization_id, project_id, source_uid) WHERE NOT retired;

CREATE INDEX tasks_by_parent ON vault.tasks (organization_id, project_id, parent_id);

CREATE TABLE vault.resources (
    organization_id uuid NOT NULL,
    project_id uuid NOT NULL,
    id uuid NOT NULL,
    source_uid integer,
    position integer,
    properties jsonb,
    model_data bytea,
    retired boolean NOT NULL DEFAULT false,
    PRIMARY KEY (organization_id, project_id, id),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects,
    CHECK (retired OR (position IS NOT NULL AND position >= 0
                       AND properties IS NOT NULL AND jsonb_typeof(properties) = 'object'
                       AND model_data IS NOT NULL AND octet_length(model_data) > 0))
);

CREATE UNIQUE INDEX live_resource_uid
    ON vault.resources (organization_id, project_id, source_uid) WHERE NOT retired;

CREATE TABLE vault.assignments (
    organization_id uuid NOT NULL,
    project_id uuid NOT NULL,
    id uuid NOT NULL,
    task_id uuid,
    resource_id uuid,
    source_uid integer,
    position integer,
    properties jsonb,
    model_data bytea,
    retired boolean NOT NULL DEFAULT false,
    PRIMARY KEY (organization_id, project_id, id),
    UNIQUE (organization_id, project_id, id, task_id),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects,
    FOREIGN KEY (organization_id, project_id, task_id) REFERENCES vault.tasks,
    FOREIGN KEY (organization_id, project_id, resource_id) REFERENCES vault.resources,
    CHECK (retired OR (position IS NOT NULL AND position >= 0
                       AND properties IS NOT NULL AND jsonb_typeof(properties) = 'object'
                       AND model_data IS NOT NULL AND octet_length(model_data) > 0))
);

CREATE UNIQUE INDEX live_assignment_uid
    ON vault.assignments (organization_id, project_id, source_uid) WHERE NOT retired;

CREATE INDEX assignments_by_task ON vault.assignments (organization_id, project_id, task_id);

CREATE INDEX assignments_by_resource ON vault.assignments (organization_id, project_id, resource_id);

-- ── Resources and time ────────────────────────────────────────────────────

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
    task_name_at_entry text,
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

-- ── Operations and audit ──────────────────────────────────────────────────

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

-- Organization audit entries name the platform operator behind a service
-- account action. Ordinary sessions reset the setting, so it stays NULL.
CREATE TABLE vault.audit_events (
    organization_id uuid NOT NULL,
    id bigint GENERATED ALWAYS AS IDENTITY,
    project_id uuid,
    actor_id uuid NOT NULL,
    operation text NOT NULL CHECK (btrim(operation) <> ''),
    occurred_at timestamptz NOT NULL DEFAULT transaction_timestamp(),
    details jsonb NOT NULL DEFAULT '{}' CHECK (jsonb_typeof(details) = 'object'),
    platform_actor_id uuid REFERENCES vault.users
        DEFAULT vault.context_id('app.platform_actor_id'),
    PRIMARY KEY (organization_id, id),
    FOREIGN KEY (organization_id, project_id) REFERENCES vault.projects,
    FOREIGN KEY (organization_id, actor_id) REFERENCES vault.organization_members
);

CREATE INDEX audit_events_by_project
    ON vault.audit_events (organization_id, project_id, occurred_at DESC);

CREATE INDEX audit_events_by_actor
    ON vault.audit_events (organization_id, actor_id, occurred_at DESC);

-- ── Platform administration ───────────────────────────────────────────────

-- One service account per organization. Platform operators act through it, so
-- every organization policy applies to them without a personal membership.
CREATE TABLE vault.organization_service_accounts (
    organization_id uuid PRIMARY KEY REFERENCES vault.organizations,
    user_id uuid NOT NULL UNIQUE,
    FOREIGN KEY (organization_id, user_id) REFERENCES vault.organization_members
);

CREATE TABLE vault.platform_operators (
    user_id uuid PRIMARY KEY REFERENCES vault.users,
    active boolean NOT NULL DEFAULT true,
    created_at timestamptz NOT NULL DEFAULT transaction_timestamp()
);

-- Platform activity that isn't tied to one organization's audit log.
CREATE TABLE vault_admin.platform_events (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    recorded_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    operator_id uuid,
    operator_login name NOT NULL,
    organization_id uuid,
    operation text NOT NULL CHECK (btrim(operation) <> ''),
    details jsonb NOT NULL DEFAULT '{}' CHECK (jsonb_typeof(details) = 'object')
);

-- ── Sign-in ───────────────────────────────────────────────────────────────

-- How each organization's people sign in: with passwords (the default), or
-- with single sign-on through the organization's identity provider.
-- provider: the Supabase Auth provider ("azure", "google", "keycloak", ...).
-- directory: what identifies the organization's accounts at that provider:
-- the Entra tenant ID for azure, the Workspace domain for google, and the
-- identity provider's issuer URL otherwise.
CREATE TABLE vault.organization_sign_in (
    organization_id uuid PRIMARY KEY REFERENCES vault.organizations ON DELETE CASCADE,
    method text NOT NULL DEFAULT 'password' CHECK (method IN ('password', 'sso')),
    provider text CHECK (provider IS NULL OR provider ~ '^[a-z0-9][a-z0-9_.-]{0,63}$'),
    updated_at timestamptz NOT NULL DEFAULT now(),
    directory text CHECK (directory IS NULL OR (btrim(directory) <> '' AND length(directory) <= 255)),
    CHECK (method = 'password' OR provider IS NOT NULL),
    CHECK (method = 'password' OR directory IS NOT NULL)
);

-- Broker logins. Only the owner's functions read this table; no one else is
-- granted it.
CREATE TABLE vault.sign_in_brokers (
    database_role_id oid PRIMARY KEY,
    database_role_name name NOT NULL UNIQUE,
    added_at timestamptz NOT NULL DEFAULT now()
);

-- ── Row-level security ────────────────────────────────────────────────────

-- FORCE applies the policies to the schema owner too.
ALTER TABLE vault.organizations ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.users ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.organization_members ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.groups ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.group_members ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.manager_principals ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.projects ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.project_import_sources ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.project_members ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.project_group_shares ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.schedule_data ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.tasks ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.resources ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.assignments ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.resource_user_mappings ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.time_entries ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.time_entry_events ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.transfer_jobs ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.audit_events ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.organization_service_accounts ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.platform_operators ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.organization_sign_in ENABLE ROW LEVEL SECURITY, FORCE ROW LEVEL SECURITY;

-- The identity resolver can use this self-only policy when binding a request.
-- App users never receive the database credential that sets request context.
CREATE POLICY read_active_identity ON vault.users FOR SELECT
    USING (active AND id = vault.context_id('app.current_user_id'));

CREATE POLICY read_own_membership ON vault.organization_members FOR SELECT
    USING (
        active
        AND organization_id = vault.context_id('app.current_organization_id')
        AND user_id = vault.context_id('app.current_user_id')
        AND EXISTS (SELECT 1 FROM vault.users WHERE id = user_id)
    );

CREATE POLICY read_own_organization ON vault.organizations FOR SELECT
    USING (
        id = vault.context_id('app.current_organization_id')
        AND EXISTS (
            SELECT 1 FROM vault.organization_members AS member
            WHERE member.organization_id = organizations.id
              AND member.user_id = vault.context_id('app.current_user_id')
        )
    );

CREATE POLICY read_own_group_memberships ON vault.group_members FOR SELECT
    USING (
        organization_id = vault.context_id('app.current_organization_id')
        AND user_id = vault.context_id('app.current_user_id')
        AND EXISTS (
            SELECT 1 FROM vault.organization_members AS member
            WHERE member.organization_id = group_members.organization_id
              AND member.user_id = group_members.user_id
        )
    );

CREATE POLICY read_joined_groups ON vault.groups FOR SELECT
    USING (
        organization_id = vault.context_id('app.current_organization_id')
        AND EXISTS (
            SELECT 1 FROM vault.group_members AS member
            WHERE member.organization_id = groups.organization_id
              AND member.group_id = groups.id
              AND member.user_id = vault.context_id('app.current_user_id')
        )
    );
