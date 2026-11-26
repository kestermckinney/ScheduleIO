-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.desktop_named_portfolio_work(tenant uuid, team uuid DEFAULT NULL)
    RETURNS TABLE (
        project_id uuid, project_title text, user_id uuid, planned_millis numeric,
        imported_actual_millis numeric, remaining_millis numeric, approved_millis numeric,
        user_display_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        item record;
        tenant_name text;
        identity_name text;
        names jsonb := '{}'::jsonb;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.desktop_portfolio_work(uuid,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Portfolio reporting permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');

        FOR item IN SELECT * FROM vault.desktop_portfolio_work(tenant, team) LOOP
            IF item.user_id IS NOT NULL AND NOT (names ? item.user_id::text) THEN
                SELECT membership.display_name INTO tenant_name
                FROM vault.organization_members AS membership
                WHERE membership.organization_id = tenant AND membership.user_id = item.user_id;

                PERFORM set_config('app.current_user_id', item.user_id::text, true);
                SELECT identity.display_name INTO identity_name
                FROM vault.users AS identity WHERE identity.id = item.user_id;
                PERFORM set_config('app.current_user_id', actor::text, true);

                names := names || jsonb_build_object(item.user_id::text,
                    coalesce(nullif(tenant_name, ''), nullif(identity_name, ''), item.user_id::text));
            END IF;
            project_id := item.project_id;
            project_title := item.project_title;
            user_id := item.user_id;
            planned_millis := item.planned_millis;
            imported_actual_millis := item.imported_actual_millis;
            remaining_millis := item.remaining_millis;
            approved_millis := item.approved_millis;
            user_display_name := names->>item.user_id::text;
            RETURN NEXT;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
