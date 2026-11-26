-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

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

CREATE FUNCTION vault.keep_assignment_endpoints() RETURNS trigger
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        IF NEW.task_id IS DISTINCT FROM OLD.task_id
            OR NEW.resource_id IS DISTINCT FROM OLD.resource_id THEN
            RAISE EXCEPTION 'Assignment endpoints cannot change' USING ERRCODE = 'check_violation';
        END IF;
        RETURN NEW;
    END
    $$;

CREATE TRIGGER keep_assignment_endpoints
    BEFORE UPDATE ON vault.assignments
    FOR EACH ROW EXECUTE FUNCTION vault.keep_assignment_endpoints();

CREATE UNIQUE INDEX live_task_uid
    ON vault.tasks (organization_id, project_id, source_uid) WHERE NOT retired;
CREATE UNIQUE INDEX live_resource_uid
    ON vault.resources (organization_id, project_id, source_uid) WHERE NOT retired;
CREATE UNIQUE INDEX live_assignment_uid
    ON vault.assignments (organization_id, project_id, source_uid) WHERE NOT retired;
CREATE INDEX tasks_by_parent ON vault.tasks (organization_id, project_id, parent_id);
CREATE INDEX assignments_by_task ON vault.assignments (organization_id, project_id, task_id);
CREATE INDEX assignments_by_resource ON vault.assignments (organization_id, project_id, resource_id);

ALTER TABLE vault.schedule_data ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.schedule_data FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.tasks ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.tasks FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.resources ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.resources FORCE ROW LEVEL SECURITY;
ALTER TABLE vault.assignments ENABLE ROW LEVEL SECURITY;
ALTER TABLE vault.assignments FORCE ROW LEVEL SECURITY;
