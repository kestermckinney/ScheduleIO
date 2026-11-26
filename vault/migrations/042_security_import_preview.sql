-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.manager_preview_security_import(tenant uuid, source jsonb) RETURNS jsonb
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        destination jsonb;
    BEGIN
        IF jsonb_typeof(source) IS DISTINCT FROM 'object'
            OR source->'format' IS DISTINCT FROM '1'::jsonb
            OR jsonb_typeof(source->'members') IS DISTINCT FROM 'array'
            OR jsonb_typeof(source->'groups') IS DISTINCT FROM 'array'
            OR jsonb_typeof(source->'projects') IS DISTINCT FROM 'array'
        THEN
            RAISE EXCEPTION 'Expected a format 1 Vault security configuration'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        -- This supplies the same tenant and owner authorization as export.
        destination := vault.manager_export_security(tenant);

        RETURN jsonb_build_object(
            'destination_organization_id', tenant,
            'members', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'source_user_id', original->'user_id',
                    'destination_user_id', target->'user_id',
                    'issuer', original->'issuer',
                    'subject', original->'subject',
                    'requested_role', original->'role',
                    'requested_active', original->'active',
                    'current_role', target->'role',
                    'current_active', target->'active'
                ) ORDER BY original->>'user_id')
                FROM jsonb_array_elements(source->'members') AS original
                LEFT JOIN jsonb_array_elements(destination->'members') AS target
                    ON original->>'issuer' = target->>'issuer'
                   AND original->>'subject' = target->>'subject'
            ), '[]'::jsonb),
            'groups', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'source_group_id', original->'group_id',
                    'destination_group_id', target->'group_id',
                    'source_name', original->'name',
                    'destination_name', target->'name',
                    'requested_members', original->'members',
                    'current_members', target->'members'
                ) ORDER BY original->>'group_id')
                FROM jsonb_array_elements(source->'groups') AS original
                LEFT JOIN jsonb_array_elements(destination->'groups') AS target
                    ON source #>> ARRAY['destination_groups', original->>'group_id'] = target->>'group_id'
            ), '[]'::jsonb),
            'projects', coalesce((
                SELECT jsonb_agg(jsonb_build_object(
                    'source_project_id', original->'project_id',
                    'destination_project_id', target->'project_id',
                    'source_title', original->'title',
                    'destination_title', target->'title',
                    'requested_user_grants', original->'user_grants',
                    'current_user_grants', target->'user_grants',
                    'requested_group_grants', original->'group_grants',
                    'current_group_grants', target->'group_grants'
                ) ORDER BY original->>'project_id')
                FROM jsonb_array_elements(source->'projects') AS original
                LEFT JOIN jsonb_array_elements(destination->'projects') AS target
                    ON source #>> ARRAY['destination_projects', original->>'project_id'] = target->>'project_id'
            ), '[]'::jsonb)
        );
    END
    $$;
