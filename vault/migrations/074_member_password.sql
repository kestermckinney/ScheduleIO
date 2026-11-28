-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- An organization administrator sets a new password for one of their own
-- people. Manager sends only a SCRAM verifier, so the password never reaches
-- the database. Organizations that sign in with single sign-on don't use
-- passwords, and platform administrators keep their own.

CREATE FUNCTION vault.manager_set_member_password(tenant uuid, member uuid, password_verifier text)
    RETURNS void
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        home uuid;
        member_login name;
    BEGIN
        IF NOT has_function_privilege(session_user,
                'vault.manager_enroll_member(uuid,text,name,boolean,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF password_verifier IS NULL
           OR password_verifier !~ '^SCRAM-SHA-256\$[0-9]+:[A-Za-z0-9+/=]+\$[A-Za-z0-9+/=]+:[A-Za-z0-9+/=]+$' THEN
            RAISE EXCEPTION 'A SCRAM-SHA-256 password verifier is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF EXISTS (SELECT FROM vault.organization_sign_in AS setting
                   WHERE setting.organization_id = tenant AND setting.method = 'sso') THEN
            RAISE EXCEPTION 'People in this organization sign in with single sign-on, not passwords'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        actor := vault.context_id('app.current_user_id');
        PERFORM set_config('app.current_user_id', member::text, true);
        SELECT identity.home_organization_id INTO home
        FROM vault.users AS identity WHERE identity.id = member AND identity.active;
        PERFORM set_config('app.current_user_id', actor::text, true);
        SELECT login.rolname INTO member_login
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login
          ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
        WHERE principal.user_id = member AND principal.active AND login.rolcanlogin
        ORDER BY login.rolname LIMIT 1;
        IF home IS DISTINCT FROM tenant OR member_login IS NULL OR vault.is_service_account(member)
           OR EXISTS (SELECT FROM vault.platform_operators AS operator WHERE operator.user_id = member) THEN
            RAISE EXCEPTION 'Select an active member of this organization with a Vault login'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        EXECUTE format('ALTER ROLE %I PASSWORD %L VALID UNTIL %L', member_login, password_verifier, 'infinity');
        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, actor, 'organization.member_password_set',
                jsonb_build_object('user_id', member, 'login', member_login));
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;
REVOKE ALL ON FUNCTION vault.manager_set_member_password(uuid, uuid, text) FROM PUBLIC;

-- Member administrators get the new entry point.
DO $migration$
DECLARE
    body text;
    login name;
BEGIN
    SELECT pg_get_functiondef('vault.grant_desktop_capability_privileges(name,text)'::regprocedure) INTO body;
    body := replace(body, '''vault.manager_set_sign_in_email(uuid,uuid,text)''',
                          '''vault.manager_set_sign_in_email(uuid,uuid,text)'',
                    ''vault.manager_set_member_password(uuid,uuid,text)''');
    IF body NOT LIKE '%manager_set_member_password%' THEN
        RAISE EXCEPTION 'Could not update vault.grant_desktop_capability_privileges';
    END IF;
    EXECUTE body;

    FOR login IN
        SELECT role.rolname
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS role
          ON role.oid = principal.database_role_id AND role.rolname = principal.database_role_name
        WHERE has_function_privilege(role.oid, 'vault.manager_set_sign_in_email(uuid,uuid,text)', 'EXECUTE')
    LOOP
        EXECUTE format('GRANT EXECUTE ON FUNCTION vault.manager_set_member_password(uuid,uuid,text) TO %I', login);
    END LOOP;
END
$migration$;
