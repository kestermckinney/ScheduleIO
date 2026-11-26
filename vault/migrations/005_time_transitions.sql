-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.guard_time_entry_update() RETURNS trigger
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        actor uuid := vault.context_id('app.current_user_id');
    BEGIN
        IF ROW(NEW.organization_id, NEW.project_id, NEW.id, NEW.user_id,
               NEW.task_id, NEW.assignment_id)
            IS DISTINCT FROM
           ROW(OLD.organization_id, OLD.project_id, OLD.id, OLD.user_id,
               OLD.task_id, OLD.assignment_id) THEN
            RAISE EXCEPTION 'Time entry identity cannot change' USING ERRCODE = 'check_violation';
        END IF;

        IF OLD.status IN ('draft', 'returned') AND actor = OLD.user_id
            AND NEW.status IN ('draft', 'submitted') THEN
            RETURN NEW;
        END IF;

        IF OLD.status = 'submitted' AND actor <> OLD.user_id
            AND NEW.status IN ('approved', 'returned')
            AND ROW(NEW.work_date, NEW.work_millis, NEW.comments, NEW.billable)
                IS NOT DISTINCT FROM
                ROW(OLD.work_date, OLD.work_millis, OLD.comments, OLD.billable) THEN
            RETURN NEW;
        END IF;

        RAISE EXCEPTION 'Invalid time entry transition' USING ERRCODE = 'check_violation';
    END
    $$;

CREATE TRIGGER guard_time_entry_update
    BEFORE UPDATE ON vault.time_entries
    FOR EACH ROW EXECUTE FUNCTION vault.guard_time_entry_update();

CREATE POLICY update_visible_time_entries ON vault.time_entries FOR UPDATE
    USING (
        vault.can_read_time_entry(organization_id, project_id, user_id)
        AND (
            (user_id = vault.context_id('app.current_user_id') AND status IN ('draft', 'returned'))
            OR (user_id <> vault.context_id('app.current_user_id') AND status = 'submitted')
        )
    )
    WITH CHECK (
        vault.can_read_time_entry(organization_id, project_id, user_id)
        AND (
            (user_id = vault.context_id('app.current_user_id') AND status IN ('draft', 'submitted'))
            OR (user_id <> vault.context_id('app.current_user_id') AND status IN ('approved', 'returned'))
        )
    );
