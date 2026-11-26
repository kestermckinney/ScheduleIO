-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY lookup_import_sources ON vault.project_import_sources FOR SELECT TO %I
         USING (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND EXISTS (
                 SELECT FROM vault.organization_members AS member
                 WHERE member.organization_id = project_import_sources.organization_id
                   AND member.user_id = vault.context_id(''app.current_user_id'')
             )
         )', current_user);
END
$migration$;

CREATE FUNCTION vault.lookup_import_source(
    tenant uuid, digest text, external_namespace text, external_key text)
    RETURNS TABLE (outcome text, project_id uuid)
    LANGUAGE plpgsql STABLE SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        matches uuid[];
    BEGIN
        IF tenant IS DISTINCT FROM vault.context_id('app.current_organization_id')
            OR NOT EXISTS (
                SELECT FROM vault.organization_members AS member
                WHERE member.organization_id = tenant
                  AND member.user_id = vault.context_id('app.current_user_id')
            ) THEN
            RAISE EXCEPTION 'Active organization membership required'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        IF digest IS NULL OR digest !~ '^[0-9a-f]{64}$'
            OR (external_namespace IS NULL) <> (external_key IS NULL)
            OR external_namespace = 'sha256' THEN
            RAISE EXCEPTION 'Invalid import source identifiers' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        SELECT array_agg(DISTINCT binding.project_id) INTO matches
        FROM vault.project_import_sources AS binding
        WHERE binding.organization_id = tenant
          AND ((binding.source_namespace = 'sha256' AND binding.source_key = digest)
              OR (binding.source_namespace = external_namespace AND binding.source_key = external_key));

        IF coalesce(cardinality(matches), 0) = 0 THEN
            RETURN QUERY SELECT 'unrecognized'::text, NULL::uuid;
        ELSIF EXISTS (
            SELECT FROM unnest(matches) AS match(id)
            WHERE NOT EXISTS (
                SELECT FROM vault.projects AS project
                WHERE project.organization_id = tenant AND project.id = match.id
            )
        ) THEN
            RETURN QUERY SELECT 'restricted'::text, NULL::uuid;
        ELSIF cardinality(matches) > 1 THEN
            RETURN QUERY SELECT 'ambiguous'::text, NULL::uuid;
        ELSE
            RETURN QUERY SELECT 'matched'::text, matches[1];
        END IF;
    END
    $$;

REVOKE ALL ON FUNCTION vault.lookup_import_source(uuid, text, text, text) FROM PUBLIC;
