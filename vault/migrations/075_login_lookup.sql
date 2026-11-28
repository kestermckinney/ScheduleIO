-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- Signing in with an email. PostgreSQL needs a login name before anyone can
-- connect, so the apps first connect as this Vault's lookup login,
-- "<database>_lookup", whose password is its own name. It may only run
-- vault.login_for_email(), which returns the login of the active person with
-- that sign-in email (or nothing), and then sign in as that person with
-- their own password. Anyone with the lookup login can learn whether an
-- email has a login here; it opens nothing else.

-- True only inside login_for_email, in a session of the lookup login.
CREATE FUNCTION vault.in_login_lookup()
    RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT coalesce(current_setting('app.login_lookup', true), '') = 'on'
           AND session_user = (current_database() || '_lookup')::name
    $$;
REVOKE ALL ON FUNCTION vault.in_login_lookup() FROM PUBLIC;

DO $migration$
BEGIN
    EXECUTE format('CREATE POLICY lookup_read_people ON vault.users FOR SELECT TO %I
                    USING (vault.in_login_lookup())', current_user);
    EXECUTE format('CREATE POLICY lookup_read_principals ON vault.manager_principals FOR SELECT TO %I
                    USING (vault.in_login_lookup())', current_user);
END
$migration$;

CREATE FUNCTION vault.login_for_email(email text)
    RETURNS name
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        wanted text := nullif(btrim(lower(email)), '');
        found name;
        matches integer;
    BEGIN
        IF wanted IS NULL THEN
            RETURN NULL;
        END IF;
        PERFORM set_config('app.login_lookup', 'on', true);
        SELECT min(principal.database_role_name), count(DISTINCT principal.database_role_name)
        INTO found, matches
        FROM vault.users AS account
        JOIN vault.manager_principals AS principal ON principal.user_id = account.id AND principal.active
        JOIN pg_roles AS login
          ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
        WHERE account.sign_in_email = wanted AND account.active AND login.rolcanlogin
          AND NOT vault.is_service_account(account.id);
        PERFORM set_config('app.login_lookup', '', true);
        -- Two people sharing an email can't be told apart; sign in with the login.
        RETURN CASE WHEN matches = 1 THEN found END;
    END
    $$;
REVOKE ALL ON FUNCTION vault.login_for_email(text) FROM PUBLIC;

-- The lookup login. Creating it needs CREATEROLE, which the Vault owner has
-- when Schedule Vault Manager made the Vault; otherwise a server administrator
-- can create it and run the grants below.
DO $migration$
DECLARE
    lookup name := (current_database() || '_lookup')::name;
BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = lookup) THEN
        IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = current_user AND rolcreaterole) THEN
            RAISE NOTICE 'Signing in with an email needs the login %; the Vault owner can''t create it', lookup;
            RETURN;
        END IF;
        EXECUTE format('CREATE ROLE %I LOGIN PASSWORD %L NOSUPERUSER NOBYPASSRLS NOCREATEDB '
                       'NOCREATEROLE NOREPLICATION', lookup, lookup);
    END IF;
    EXECUTE format('GRANT CONNECT ON DATABASE %I TO %I', current_database(), lookup);
    EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', lookup);
    EXECUTE format('GRANT EXECUTE ON FUNCTION vault.login_for_email(text) TO %I', lookup);
END
$migration$;
