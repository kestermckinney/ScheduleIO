-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.desktop_organization_work_date(tenant uuid) RETURNS date
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        today date;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.desktop_list_time(uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Vault Time permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        SELECT (statement_timestamp() AT TIME ZONE organization.time_zone)::date INTO today
        FROM vault.organizations AS organization WHERE organization.id = tenant;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN today;
    END
    $$;
