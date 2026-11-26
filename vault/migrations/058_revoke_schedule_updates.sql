-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.revoke_desktop_schedule_updates(desktop_login text)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        login_id oid;
    BEGIN
        IF NOT EXISTS (
            SELECT FROM pg_namespace AS schema
            JOIN pg_roles AS owner ON owner.oid = schema.nspowner
            WHERE schema.nspname = 'vault' AND owner.rolname = current_user
              AND NOT owner.rolsuper AND NOT owner.rolbypassrls
        ) THEN
            RAISE EXCEPTION 'Connect as the Vault schema owner'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
        SELECT login.oid INTO login_id
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login
          ON (login.oid, login.rolname) = (principal.database_role_id, principal.database_role_name)
        WHERE login.rolname::text = desktop_login;
        IF login_id IS NULL THEN
            RAISE EXCEPTION 'Select a registered Vault login' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        EXECUTE format(
            'REVOKE EXECUTE ON FUNCTION
                vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb),
                vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)
             FROM %I', desktop_login);

        -- Reject an apparently successful revocation if another database role
        -- still grants the same capability. The exception rolls back the change.
        IF EXISTS (
            SELECT FROM pg_roles AS candidate WHERE pg_has_role(login_id, candidate.oid, 'MEMBER')
              AND (has_function_privilege(candidate.oid,
                    'vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb)', 'EXECUTE')
                   OR has_function_privilege(candidate.oid,
                    'vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)', 'EXECUTE'))
        ) THEN
            RAISE EXCEPTION 'Another PostgreSQL role still provides schedule updates to this login'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
    END
    $$;

REVOKE ALL ON FUNCTION vault.revoke_desktop_schedule_updates(text) FROM PUBLIC;
