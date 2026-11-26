-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- FORCE RLS applies to the migration owner too. Allow the trigger's owner
-- to append the event it derives from a successful time entry write.
DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY append_time_entry_events ON vault.time_entry_events
         FOR INSERT TO %I WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
         )',
        current_user
    );
END
$migration$;

CREATE FUNCTION vault.record_time_entry_event() RETURNS trigger
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        event text;
    BEGIN
        IF TG_OP = 'INSERT' THEN
            event := 'created';
        ELSIF NEW.status IN ('submitted', 'approved', 'returned')
            AND NEW.status IS DISTINCT FROM OLD.status THEN
            event := NEW.status;
        ELSE
            RETURN NULL;
        END IF;

        INSERT INTO vault.time_entry_events
            (organization_id, project_id, entry_id, actor_id, event_type)
        VALUES (NEW.organization_id, NEW.project_id, NEW.id,
                vault.context_id('app.current_user_id'), event);

        RETURN NULL;
    END
    $$;

REVOKE ALL ON FUNCTION vault.record_time_entry_event() FROM PUBLIC;

CREATE TRIGGER record_time_entry_event
    AFTER INSERT OR UPDATE ON vault.time_entries
    FOR EACH ROW EXECUTE FUNCTION vault.record_time_entry_event();
