-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.manager_apply_security_import(tenant uuid, source jsonb, reviewed jsonb)
    RETURNS integer
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        plan jsonb;
        users_by_source jsonb;
        groups_by_source jsonb;
        item jsonb;
        grant_row jsonb;
        target uuid;
        member uuid;
        requested_role text;
        requested_active boolean;
        changes integer := 0;
    BEGIN
        IF current_setting('transaction_isolation') <> 'serializable' THEN
            RAISE EXCEPTION 'Apply security configuration in a serializable transaction'
                USING ERRCODE = 'invalid_transaction_state';
        END IF;

        plan := vault.manager_preview_security_import(tenant, source);
        IF plan IS DISTINCT FROM reviewed THEN
            RAISE EXCEPTION 'Security configuration changed. Review a new preview before applying.'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        IF NOT has_function_privilege(session_user, 'vault.manager_change_group(uuid,uuid,text,text,uuid)', 'EXECUTE')
            OR NOT has_function_privilege(session_user,
                'vault.manager_change_project_share(uuid,uuid,text,uuid,text,boolean)', 'EXECUTE')
        THEN
            RAISE EXCEPTION 'Group and project sharing administration permissions required'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        IF EXISTS (SELECT FROM jsonb_array_elements(plan->'members') AS row_data
                   WHERE row_data->>'destination_user_id' IS NULL)
            OR EXISTS (SELECT FROM jsonb_array_elements(plan->'groups') AS row_data
                       WHERE row_data->>'destination_group_id' IS NULL)
            OR EXISTS (SELECT FROM jsonb_array_elements(plan->'projects') AS row_data
                       WHERE row_data->>'destination_project_id' IS NULL)
        THEN
            RAISE EXCEPTION 'Every source member, group, and project must have a destination'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        IF (SELECT count(DISTINCT row_data->>'source_user_id') <> count(*)
                   OR count(DISTINCT row_data->>'destination_user_id') <> count(*)
            FROM jsonb_array_elements(plan->'members') AS row_data)
            OR (SELECT count(DISTINCT row_data->>'source_group_id') <> count(*)
                       OR count(DISTINCT row_data->>'destination_group_id') <> count(*)
                FROM jsonb_array_elements(plan->'groups') AS row_data)
            OR (SELECT count(DISTINCT row_data->>'source_project_id') <> count(*)
                       OR count(DISTINCT row_data->>'destination_project_id') <> count(*)
                FROM jsonb_array_elements(plan->'projects') AS row_data)
        THEN
            RAISE EXCEPTION 'Security configuration mappings must be one-to-one'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        IF EXISTS (SELECT FROM jsonb_array_elements(plan->'groups') AS row_data
                   WHERE jsonb_typeof(row_data->'requested_members') IS DISTINCT FROM 'array')
            OR EXISTS (SELECT FROM jsonb_array_elements(plan->'projects') AS row_data
                       WHERE jsonb_typeof(row_data->'requested_user_grants') IS DISTINCT FROM 'array'
                          OR jsonb_typeof(row_data->'requested_group_grants') IS DISTINCT FROM 'array')
        THEN
            RAISE EXCEPTION 'Expected arrays for group members and project grants'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        SELECT jsonb_object_agg(row_data->>'source_user_id', row_data->>'destination_user_id')
        INTO users_by_source FROM jsonb_array_elements(plan->'members') AS row_data;
        SELECT jsonb_object_agg(row_data->>'source_group_id', row_data->>'destination_group_id')
        INTO groups_by_source FROM jsonb_array_elements(plan->'groups') AS row_data;

        FOR item IN SELECT value FROM jsonb_array_elements(plan->'members') LOOP
            requested_role := item->>'requested_role';
            requested_active := (item->>'requested_active')::boolean;
            IF requested_role IS NULL OR requested_role NOT IN ('owner', 'admin', 'member')
                OR jsonb_typeof(item->'requested_active') IS DISTINCT FROM 'boolean'
            THEN
                RAISE EXCEPTION 'Invalid member role or status' USING ERRCODE = 'invalid_parameter_value';
            END IF;
            IF requested_role = 'owner' OR item->>'current_role' = 'owner' THEN
                CONTINUE;
            END IF;

            member := (item->>'destination_user_id')::uuid;
            IF requested_active AND NOT (item->>'current_active')::boolean THEN
                IF NOT vault.manager_set_member_active(tenant, member, true) THEN
                    RAISE EXCEPTION 'Could not reactivate destination member %', member;
                END IF;
                changes := changes + 1;
            END IF;
            IF requested_role <> item->>'current_role' THEN
                IF NOT vault.manager_set_member_role(tenant, member, requested_role) THEN
                    RAISE EXCEPTION 'Could not change destination member role for %', member;
                END IF;
                changes := changes + 1;
            END IF;
            IF NOT requested_active AND (item->>'current_active')::boolean THEN
                IF NOT vault.manager_set_member_active(tenant, member, false) THEN
                    RAISE EXCEPTION 'Could not deactivate destination member %', member;
                END IF;
                changes := changes + 1;
            END IF;
        END LOOP;

        FOR item IN SELECT value FROM jsonb_array_elements(plan->'groups') LOOP
            target := (item->>'destination_group_id')::uuid;
            FOR member IN
                SELECT (users_by_source->>source_id)::uuid
                FROM jsonb_array_elements_text(item->'requested_members') AS source_id
            LOOP
                IF vault.manager_change_group(tenant, target, 'add_member', NULL, member) THEN
                    changes := changes + 1;
                END IF;
            END LOOP;
        END LOOP;

        FOR item IN SELECT value FROM jsonb_array_elements(plan->'projects') LOOP
            target := (item->>'destination_project_id')::uuid;
            FOR grant_row IN SELECT value FROM jsonb_array_elements(item->'requested_user_grants') LOOP
                IF vault.manager_change_project_share(tenant, target, 'user',
                        (users_by_source->>(grant_row->>'user_id'))::uuid, grant_row->>'role', true) THEN
                    changes := changes + 1;
                END IF;
            END LOOP;
            FOR grant_row IN SELECT value FROM jsonb_array_elements(item->'requested_group_grants') LOOP
                IF vault.manager_change_project_share(tenant, target, 'group',
                        (groups_by_source->>(grant_row->>'group_id'))::uuid, grant_row->>'role', true) THEN
                    changes := changes + 1;
                END IF;
            END LOOP;
        END LOOP;

        RETURN changes;
    END
    $$;
