-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE SCHEMA vault;

CREATE TABLE vault.organizations (
    id uuid PRIMARY KEY,
    name text NOT NULL CHECK (btrim(name) <> ''),
    time_zone text NOT NULL DEFAULT 'UTC'
);

-- Only privileged identity management may access global identities directly.
CREATE TABLE vault.users (
    id uuid PRIMARY KEY,
    issuer text NOT NULL CHECK (btrim(issuer) <> ''),
    subject text NOT NULL CHECK (btrim(subject) <> ''),
    display_name text NOT NULL,
    active boolean NOT NULL DEFAULT true,
    UNIQUE (issuer, subject)
);

CREATE TABLE vault.organization_members (
    organization_id uuid NOT NULL REFERENCES vault.organizations,
    user_id uuid NOT NULL REFERENCES vault.users,
    role text NOT NULL CHECK (role IN ('owner', 'admin', 'member')),
    active boolean NOT NULL DEFAULT true,
    PRIMARY KEY (organization_id, user_id)
);

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

ALTER TABLE vault.organizations ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.organizations FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.users ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.users FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.organization_members ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.organization_members FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.groups ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.groups FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.group_members ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.group_members FORCE ROW LEVEL SECURITY;

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
