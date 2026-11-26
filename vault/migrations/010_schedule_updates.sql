-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- The ScheduleVault write service receives UPDATE on these columns during
-- provisioning. Manager's initial-import role does not receive that grant.
CREATE POLICY update_live_project ON vault.projects FOR UPDATE
    USING (NOT archived AND vault.has_project_role(organization_id, id, ARRAY['manager']))
    WITH CHECK (NOT archived AND vault.has_project_role(organization_id, id, ARRAY['manager']));

CREATE FUNCTION vault.begin_schedule_update(tenant uuid, project uuid, expected_edit_counter bigint,
                                             new_title text)
    RETURNS bigint
    LANGUAGE sql VOLATILE
    SET search_path = pg_catalog
    AS $$
        UPDATE vault.projects
           SET edit_counter = edit_counter + 1, title = new_title
         WHERE organization_id = tenant AND id = project
           AND edit_counter = expected_edit_counter
         RETURNING edit_counter
    $$;

REVOKE ALL ON FUNCTION vault.begin_schedule_update(uuid, uuid, bigint, text) FROM PUBLIC;

CREATE FUNCTION vault.guard_project_edit_counter() RETURNS trigger
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        IF NEW.edit_counter <> OLD.edit_counter + 1 THEN
            RAISE EXCEPTION 'Project edit counter must advance by one'
                USING ERRCODE = 'check_violation';
        END IF;
        RETURN NEW;
    END
    $$;

CREATE TRIGGER guard_project_edit_counter
    BEFORE UPDATE OF edit_counter, title ON vault.projects
    FOR EACH ROW EXECUTE FUNCTION vault.guard_project_edit_counter();

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY append_schedule_update_audit ON vault.audit_events
         FOR INSERT TO %I WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''schedule.updated''
         )',
        current_user
    );
END
$migration$;

CREATE FUNCTION vault.record_schedule_update() RETURNS trigger
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    BEGIN
        INSERT INTO vault.audit_events (organization_id, project_id, actor_id, operation, details)
        VALUES (NEW.organization_id, NEW.id, vault.context_id('app.current_user_id'),
                'schedule.updated', jsonb_build_object('edit_counter', NEW.edit_counter));
        RETURN NULL;
    END
    $$;

REVOKE ALL ON FUNCTION vault.record_schedule_update() FROM PUBLIC;

CREATE TRIGGER record_schedule_update
    AFTER UPDATE OF edit_counter ON vault.projects
    FOR EACH ROW EXECUTE FUNCTION vault.record_schedule_update();
