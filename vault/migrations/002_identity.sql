-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE FUNCTION vault.bind_manager_context(tenant uuid)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        actor uuid;
        operator uuid;
        service uuid;
        tenant_status text;
        signed_in text := coalesce(system_user, '');
        token_identity text;
        token_issuer text;
        token_subject text;
        person_issuer text;
        person_subject text;
    BEGIN
        IF current_user IN (SELECT rolname FROM pg_roles WHERE rolsuper OR rolbypassrls) THEN
            RAISE EXCEPTION 'Desktop Vault operations require a migration owner subject to RLS'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        SELECT principal.user_id INTO actor
        FROM vault.manager_principals AS principal
        JOIN pg_roles AS login
          ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
        WHERE login.rolname = session_user AND login.rolcanlogin AND principal.active;

        IF actor IS NULL THEN
            RAISE EXCEPTION 'PostgreSQL login has no active Vault Manager identity'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        -- ── Single sign-on identity ──
        -- A token sign-in reaches here as system_user 'oauth:<issuer>#<subject>'
        -- (set by the server's OAuth validator, which only lets a token open the
        -- login its provider account was assigned). The first token sign-in
        -- records that identity on the person; every later one must match it,
        -- and a person with a recorded identity can no longer use a password.
        PERFORM set_config('app.current_user_id', actor::text, true);
        SELECT person.issuer, person.subject INTO person_issuer, person_subject
        FROM vault.users AS person WHERE person.id = actor;

        IF signed_in LIKE 'oauth:%' THEN
            token_identity := substr(signed_in, length('oauth:') + 1);
            IF person_issuer !~ '^https?://' THEN
                token_issuer := substring(token_identity FROM '^(.+)#[^#]+$');
                token_subject := substring(token_identity FROM '#([^#]+)$');
                IF token_issuer IS NULL OR token_subject IS NULL OR person_issuer = 'vault-service' THEN
                    RAISE EXCEPTION 'This sign-in can''t be linked to a Vault person'
                        USING ERRCODE = 'insufficient_privilege';
                END IF;
                PERFORM set_config('app.link_sign_in_identity', 'on', true);
                BEGIN
                    UPDATE vault.users SET issuer = token_issuer, subject = token_subject WHERE id = actor;
                EXCEPTION WHEN unique_violation THEN
                    RAISE EXCEPTION 'This single sign-on account is already linked to another Vault person'
                        USING ERRCODE = 'insufficient_privilege';
                END;
                PERFORM set_config('app.link_sign_in_identity', '', true);
            ELSIF person_issuer || '#' || person_subject IS DISTINCT FROM token_identity THEN
                RAISE EXCEPTION 'This sign-in belongs to a different person than this login'
                    USING ERRCODE = 'insufficient_privilege';
            END IF;
        ELSIF vault.lasting_password_refused(actor, session_user) THEN
            RAISE EXCEPTION 'This person signs in with single sign-on'
                USING ERRCODE = 'insufficient_privilege';
        END IF;

        operator := vault.platform_operator_id();
        IF operator IS NOT NULL THEN
            IF tenant IS NOT NULL THEN
                SELECT account.user_id INTO service
                FROM vault.organization_service_accounts AS account WHERE account.organization_id = tenant;
                IF service IS NULL THEN
                    RAISE EXCEPTION 'Organization not found' USING ERRCODE = 'invalid_parameter_value';
                END IF;
                actor := service;
            END IF;
            PERFORM set_config('app.platform_actor_id', operator::text, true);
            PERFORM set_config('app.current_organization_id', coalesce(tenant::text, ''), true);
            PERFORM set_config('app.current_user_id', actor::text, true);
            RETURN;
        END IF;

        PERFORM set_config('app.platform_actor_id', '', true);
        PERFORM set_config('app.current_organization_id', coalesce(tenant::text, ''), true);
        PERFORM set_config('app.current_user_id', actor::text, true);
        IF tenant IS NOT NULL THEN
            SELECT organization.status INTO tenant_status
            FROM vault.organizations AS organization WHERE organization.id = tenant;
            IF tenant_status = 'suspended' THEN
                RAISE EXCEPTION 'This organization is suspended. Contact your service provider.'
                    USING ERRCODE = 'insufficient_privilege';
            END IF;
        END IF;
    END
    $$;

REVOKE ALL ON FUNCTION vault.bind_manager_context(uuid) FROM PUBLIC;

CREATE FUNCTION vault.manager_import_initial_schedule(
    tenant uuid, new_project uuid, title text, source_namespace text, source_key text,
    content_digest text, metadata bytea, collections bytea,
    task_rows jsonb, resource_rows jsonb, assignment_rows jsonb)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        imported uuid;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        imported := vault.import_initial_schedule(
            tenant, new_project, title, source_namespace, source_key, content_digest,
            metadata, collections, task_rows, resource_rows, assignment_rows);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN imported;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_import_initial_schedule(
    uuid, uuid, text, text, text, text, bytea, bytea, jsonb, jsonb, jsonb) FROM PUBLIC;

CREATE FUNCTION vault.manager_lookup_import_source(
    tenant uuid, digest text, external_namespace text, external_key text)
    RETURNS TABLE (outcome text, project_id uuid)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        RETURN QUERY SELECT lookup.outcome, lookup.project_id
        FROM vault.lookup_import_source(tenant, digest, external_namespace, external_key) AS lookup;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_lookup_import_source(uuid, text, text, text) FROM PUBLIC;

-- Schema-owner provisioning of an organization with its owner, for dedicated
-- instances and tests. The owner's home is the new organization.
CREATE FUNCTION vault.provision_organization(
    tenant uuid, organization_name text, organization_time_zone text,
    actor uuid, display_name text, desktop_login name,
    allow_initial_import boolean, allow_schedule_updates boolean)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    DECLARE
        login_id oid;
    BEGIN
        IF EXISTS (SELECT FROM pg_roles WHERE rolname = current_user AND (rolsuper OR rolbypassrls)) THEN
            RAISE EXCEPTION 'Provision using the Vault schema owner subject to RLS'
                USING ERRCODE = 'insufficient_privilege';
        END IF;
        SELECT oid INTO login_id FROM pg_roles
        WHERE rolname = desktop_login AND rolcanlogin AND NOT rolsuper
          AND NOT rolbypassrls AND NOT rolcreaterole AND NOT rolcreatedb
          AND NOT rolreplication AND NOT pg_has_role(oid, current_user, 'MEMBER');
        IF login_id IS NULL OR EXISTS (
            SELECT FROM vault.manager_principals AS principal WHERE principal.database_role_id = login_id
        ) THEN
            RAISE EXCEPTION 'A separate unprivileged PostgreSQL login is required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF NOT (coalesce(allow_initial_import, false) OR coalesce(allow_schedule_updates, false)) THEN
            RAISE EXCEPTION 'Select a desktop capability' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        PERFORM vault.create_organization_record(tenant, organization_name, organization_time_zone);
        INSERT INTO vault.users (id, issuer, subject, display_name, home_organization_id)
        VALUES (actor, 'postgresql', actor::text, display_name, tenant);
        INSERT INTO vault.organization_members (organization_id, user_id, role)
        VALUES (tenant, actor, 'owner');
        INSERT INTO vault.manager_principals (database_role_id, database_role_name, user_id)
        VALUES (login_id, desktop_login, actor);

        EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', desktop_login);
        EXECUTE format(
            'GRANT EXECUTE ON FUNCTION vault.manager_lookup_import_source(uuid,text,text,text) TO %I',
            desktop_login);
        IF allow_initial_import THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.manager_import_initial_schedule(
                    uuid,uuid,text,text,text,text,bytea,bytea,jsonb,jsonb,jsonb) TO %I', desktop_login);
        END IF;
        IF allow_schedule_updates THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.desktop_read_schedule(uuid,uuid),
                    vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb),
                    vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text)
                 TO %I', desktop_login);
        END IF;
    END
    $$;

REVOKE ALL ON FUNCTION vault.provision_organization(uuid,text,text,uuid,text,name,boolean,boolean)
    FROM PUBLIC;

-- This helper checks only the actor's own membership. Group policies can use
-- it without traversing project grants, whose visibility also depends on groups.
CREATE FUNCTION vault.is_organization_admin(tenant uuid) RETURNS boolean
    LANGUAGE sql STABLE
    SET search_path = pg_catalog
    AS $$
        SELECT tenant = vault.context_id('app.current_organization_id') AND EXISTS (
            SELECT FROM vault.organization_members AS member
            WHERE member.organization_id = tenant
              AND member.user_id = vault.context_id('app.current_user_id')
              AND member.role IN ('owner', 'admin')
        )
    $$;

CREATE FUNCTION vault.manager_change_group(
    tenant uuid, group_id_value uuid, operation_value text, group_name text, member_id uuid)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        event text;
        changed boolean;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        CASE operation_value
            WHEN 'create' THEN
                INSERT INTO vault.groups (organization_id, id, name)
                VALUES (tenant, group_id_value, group_name);
                event := 'group.created';
            WHEN 'rename' THEN
                IF group_name IS NULL OR btrim(group_name) = '' THEN
                    RAISE EXCEPTION 'Enter a group name' USING ERRCODE = 'invalid_parameter_value';
                END IF;
                UPDATE vault.groups AS team SET name = btrim(group_name)
                WHERE team.organization_id = tenant AND team.id = group_id_value
                  AND team.name IS DISTINCT FROM btrim(group_name);
                event := 'group.renamed';
            WHEN 'add_member' THEN
                INSERT INTO vault.group_members (organization_id, group_id, user_id)
                VALUES (tenant, group_id_value, member_id)
                ON CONFLICT DO NOTHING;
                event := 'group.member_added';
            WHEN 'remove_member' THEN
                DELETE FROM vault.group_members
                WHERE organization_id = tenant AND group_id = group_id_value AND user_id = member_id;
                event := 'group.member_removed';
            ELSE
                RAISE EXCEPTION 'Unsupported group operation' USING ERRCODE = 'invalid_parameter_value';
        END CASE;

        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, vault.context_id('app.current_user_id'), event,
                    jsonb_strip_nulls(jsonb_build_object('group_id', group_id_value, 'member_id', member_id,
                        'name', CASE WHEN operation_value = 'rename' THEN btrim(group_name) END)));
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_change_group(uuid, uuid, text, text, uuid) FROM PUBLIC;

-- ── Members stay in their home organization ───────────────────────────────

CREATE FUNCTION vault.manager_enroll_member(
    tenant uuid, display_name text, desktop_login name, allow_schedule boolean, allow_time boolean)
    RETURNS uuid
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        login_id oid;
        member_id uuid;
        home uuid;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        actor := vault.context_id('app.current_user_id');
        IF nullif(btrim(display_name), '') IS NULL
           OR NOT (coalesce(allow_schedule, false) OR coalesce(allow_time, false)) THEN
            RAISE EXCEPTION 'A member name and desktop capability are required'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        SELECT oid INTO login_id FROM pg_roles
        WHERE rolname = desktop_login AND rolcanlogin AND NOT rolsuper
          AND NOT rolbypassrls AND NOT rolcreaterole AND NOT rolcreatedb
          AND NOT rolreplication AND NOT pg_has_role(oid, current_user, 'MEMBER');
        IF login_id IS NOT NULL THEN
            PERFORM pg_advisory_xact_lock(173195, (login_id::text::bigint % 2147483647)::integer);
            SELECT principal.user_id INTO member_id
            FROM vault.manager_principals AS principal
            WHERE principal.database_role_id = login_id AND principal.database_role_name = desktop_login;
            IF member_id IS NOT NULL THEN
                PERFORM set_config('app.current_user_id', member_id::text, true);
                SELECT identity.home_organization_id INTO home
                FROM vault.users AS identity WHERE identity.id = member_id;
                PERFORM set_config('app.current_user_id', actor::text, true);
                IF home IS DISTINCT FROM tenant
                   OR NOT EXISTS (SELECT FROM vault.manager_principals AS principal
                                  WHERE principal.database_role_id = login_id AND principal.active) THEN
                    login_id := NULL;
                END IF;
            END IF;
        END IF;
        -- A missing login, one registered to another organization, and a
        -- platform login are rejected at one place with one message, so the
        -- error reveals nothing about other tenants.
        IF login_id IS NULL THEN
            RAISE EXCEPTION 'That login isn''t available. Use a new login, or one that belongs to this organization.'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        IF member_id IS NULL THEN
            member_id := gen_random_uuid();
            INSERT INTO vault.users (id, issuer, subject, display_name, home_organization_id)
            VALUES (member_id, 'postgresql', member_id::text, display_name, tenant);
            INSERT INTO vault.manager_principals (database_role_id, database_role_name, user_id)
            VALUES (login_id, desktop_login, member_id);
        END IF;

        INSERT INTO vault.organization_members (organization_id, user_id, role)
        VALUES (tenant, member_id, 'member')
        ON CONFLICT DO NOTHING;
        IF FOUND THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, actor, 'organization.member_enrolled', jsonb_build_object('user_id', member_id));
        END IF;

        EXECUTE format('GRANT USAGE ON SCHEMA vault TO %I', desktop_login);
        IF allow_schedule THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.desktop_read_schedule(uuid,uuid),
                    vault.desktop_save_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb),
                    vault.desktop_reimport_schedule(uuid,uuid,bigint,text,bytea,bytea,jsonb,jsonb,jsonb,text),
                    vault.manager_lookup_import_source(uuid,text,text,text) TO %I', desktop_login);
        END IF;
        IF allow_time THEN
            EXECUTE format(
                'GRANT EXECUTE ON FUNCTION vault.desktop_list_time_work(uuid,uuid),
                    vault.desktop_list_time(uuid,uuid),
                    vault.desktop_create_time(uuid,uuid,uuid,uuid,uuid,date,bigint,text,boolean),
                    vault.desktop_update_time(uuid,uuid,uuid,text,text,date,bigint,text,boolean)
                 TO %I', desktop_login);
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN member_id;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_enroll_member(uuid, text, name, boolean, boolean) FROM PUBLIC;

CREATE FUNCTION vault.manager_set_member_active(tenant uuid, member_id uuid, new_active boolean)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        changed boolean;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        UPDATE vault.organization_members AS member SET active = new_active
        WHERE member.organization_id = tenant AND member.user_id = member_id
          AND member.role = 'member' AND member.active IS DISTINCT FROM new_active;
        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, vault.context_id('app.current_user_id'),
                    CASE WHEN new_active THEN 'organization.member_reactivated'
                                         ELSE 'organization.member_deactivated' END,
                    jsonb_build_object('user_id', member_id));
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;

REVOKE ALL ON FUNCTION vault.manager_set_member_active(uuid, uuid, boolean) FROM PUBLIC;

CREATE FUNCTION vault.manager_set_member_role(tenant uuid, member_id uuid, new_role text)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        changed boolean;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        IF NOT EXISTS (
            SELECT FROM vault.organization_members AS membership
            WHERE membership.organization_id = tenant AND membership.user_id = actor
              AND membership.role = 'owner'
        ) THEN
            RAISE EXCEPTION 'Organization owner required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF new_role IS NULL OR new_role NOT IN ('member', 'admin') THEN
            RAISE EXCEPTION 'Select the member or administrator role' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        UPDATE vault.organization_members AS membership SET role = new_role
        WHERE membership.organization_id = tenant AND membership.user_id = member_id
          AND membership.role IN ('member', 'admin') AND membership.role <> new_role
          AND membership.active;
        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, actor, 'organization.member_role_changed',
                    jsonb_build_object('user_id', member_id, 'role', new_role));
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;

-- ── Directories hide the service account ──────────────────────────────────

CREATE FUNCTION vault.manager_list_members(tenant uuid)
    RETURNS TABLE (member_id uuid, display_name text, membership_role text, membership_active boolean)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        membership record;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        actor := vault.context_id('app.current_user_id');

        FOR membership IN
            SELECT member.user_id, member.role, member.active, member.display_name
            FROM vault.organization_members AS member
            WHERE member.organization_id = tenant AND NOT vault.is_service_account(member.user_id)
            ORDER BY NOT member.active, member.role, member.user_id
        LOOP
            PERFORM set_config('app.current_user_id', membership.user_id::text, true);
            SELECT identity.display_name INTO display_name
            FROM vault.users AS identity WHERE identity.id = membership.user_id;
            PERFORM set_config('app.current_user_id', actor::text, true);

            member_id := membership.user_id;
            membership_role := membership.role;
            membership_active := membership.active AND display_name IS NOT NULL;
            display_name := coalesce(membership.display_name, display_name);
            RETURN NEXT;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

-- Ownership: the platform can name any active member as owner, which is how a
-- new customer organization gets its first owner.
CREATE FUNCTION vault.manager_transfer_ownership(tenant uuid, new_owner uuid)
    RETURNS void
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        changed integer;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM set_config('app.ownership_transfer_target', '', true);
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        PERFORM pg_advisory_xact_lock(hashtextextended(tenant::text, 38));

        IF vault.is_platform_session(tenant) THEN
            IF new_owner IS NULL OR vault.is_service_account(new_owner) OR NOT EXISTS (
                SELECT FROM vault.organization_members AS member
                WHERE member.organization_id = tenant AND member.user_id = new_owner AND member.active
            ) THEN
                RAISE EXCEPTION 'Ownership requires an active member in this organization'
                    USING ERRCODE = 'invalid_parameter_value';
            END IF;
            UPDATE vault.organization_members AS member SET role = 'admin'
            WHERE member.organization_id = tenant AND member.role = 'owner' AND member.user_id <> new_owner
              AND NOT vault.is_service_account(member.user_id);
            UPDATE vault.organization_members AS member SET role = 'owner'
            WHERE member.organization_id = tenant AND member.user_id = new_owner;
            PERFORM vault.grant_owner_capabilities(tenant, new_owner);
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, actor, 'organization.ownership_transferred', jsonb_build_object('new_owner', new_owner));
            PERFORM set_config('app.current_organization_id', previous_tenant, true);
            PERFORM set_config('app.current_user_id', previous_actor, true);
            RETURN;
        END IF;

        IF NOT EXISTS (
            SELECT FROM vault.organization_members AS member
            WHERE member.organization_id = tenant AND member.user_id = actor
              AND member.role = 'owner' AND member.active
        ) THEN
            RAISE EXCEPTION 'Organization owner required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        IF new_owner IS NULL OR new_owner = actor OR vault.is_service_account(new_owner) OR NOT EXISTS (
            SELECT FROM vault.manager_principals AS principal
            JOIN pg_roles AS login
              ON login.oid = principal.database_role_id AND login.rolname = principal.database_role_name
            WHERE principal.user_id = new_owner AND principal.active AND login.rolcanlogin
        ) THEN
            RAISE EXCEPTION 'Select a different member with an active Vault login'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM set_config('app.current_user_id', new_owner::text, true);
        IF NOT EXISTS (SELECT FROM vault.users AS identity WHERE identity.id = new_owner AND identity.active) THEN
            RAISE EXCEPTION 'Select an active Vault user' USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM set_config('app.current_user_id', actor::text, true);
        PERFORM set_config('app.ownership_transfer_target', new_owner::text, true);

        UPDATE vault.organization_members AS member
        SET role = CASE WHEN member.user_id = actor THEN 'admin' ELSE 'owner' END
        WHERE member.organization_id = tenant AND member.active
          AND ((member.user_id = actor AND member.role = 'owner')
               OR (member.user_id = new_owner AND member.role IN ('member', 'admin')));
        GET DIAGNOSTICS changed = ROW_COUNT;
        IF changed <> 2 THEN
            RAISE EXCEPTION 'Ownership requires an active member in this organization'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;
        PERFORM vault.grant_owner_capabilities(tenant, new_owner);

        INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
        VALUES (tenant, actor, 'organization.ownership_transferred',
                jsonb_build_object('new_owner', new_owner));
        PERFORM set_config('app.ownership_transfer_target', '', true);
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

CREATE FUNCTION vault.manager_list_groups(tenant uuid, selected_group uuid)
    RETURNS TABLE (group_id uuid, group_name text, member_id uuid)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_change_group(uuid,uuid,text,text,uuid)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Group administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;

        RETURN QUERY
            SELECT team.id, team.name, member.user_id
            FROM vault.groups AS team
            LEFT JOIN vault.group_members AS member
              ON (member.organization_id, member.group_id) = (team.organization_id, team.id)
             AND selected_group IS NOT NULL
            WHERE team.organization_id = tenant AND (selected_group IS NULL OR team.id = selected_group)
            ORDER BY team.name, team.id, member.user_id;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

-- ── Organization directory ────────────────────────────────────────────────

CREATE FUNCTION vault.manager_list_organizations()
    RETURNS TABLE (organization_id uuid, organization_name text)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        tenant uuid;
    BEGIN
        IF vault.platform_operator_id() IS NOT NULL THEN
            RETURN QUERY SELECT organization.id, organization.name
                FROM vault.organizations AS organization ORDER BY lower(organization.name), organization.id;
            RETURN;
        END IF;

        -- Resolve the authenticated login and enter directory scope. Each name
        -- is then read under the ordinary single-organization policy.
        PERFORM vault.bind_manager_context(NULL);
        PERFORM set_config('app.current_organization_id', '', true);
        FOR tenant IN SELECT member.organization_id FROM vault.organization_members AS member
                      WHERE member.user_id = vault.context_id('app.current_user_id') AND member.active
                      ORDER BY member.organization_id
        LOOP
            PERFORM set_config('app.current_organization_id', tenant::text, true);
            RETURN QUERY SELECT organization.id, organization.name
                FROM vault.organizations AS organization
                WHERE organization.id = tenant AND organization.status = 'active';
        END LOOP;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

CREATE FUNCTION vault.manager_set_member_name(tenant uuid, member_id uuid, new_name text)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        actor_is_owner boolean;
        changed boolean;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Member administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        actor := vault.context_id('app.current_user_id');
        SELECT member.role = 'owner' INTO actor_is_owner
        FROM vault.organization_members AS member
        WHERE member.organization_id = tenant AND member.user_id = actor AND member.active;

        IF new_name IS NULL OR char_length(btrim(new_name)) NOT BETWEEN 1 AND 200 THEN
            RAISE EXCEPTION 'Enter a display name of 1 to 200 characters' USING ERRCODE = 'invalid_parameter_value';
        END IF;

        UPDATE vault.organization_members AS member SET display_name = btrim(new_name)
        WHERE member.organization_id = tenant AND member.user_id = member_id AND member.user_id <> actor
          AND (member.role = 'member' OR (actor_is_owner AND member.role = 'admin'))
          AND member.display_name IS DISTINCT FROM btrim(new_name);
        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, actor, 'organization.member_name_changed', jsonb_build_object('user_id', member_id));
        END IF;
        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;

CREATE FUNCTION vault.manager_list_group_roster(tenant uuid, selected_group uuid)
    RETURNS TABLE (group_id uuid, group_name text, member_id uuid, member_name text, member_active boolean)
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        actor uuid;
        roster record;
        organization_name text;
        membership_active boolean;
    BEGIN
        PERFORM vault.bind_manager_context(tenant);
        actor := vault.context_id('app.current_user_id');
        -- The directory checks both the operator capability and organization role.
        FOR roster IN SELECT * FROM vault.manager_list_groups(tenant, selected_group) LOOP
            group_id := roster.group_id;
            group_name := roster.group_name;
            member_id := roster.member_id;
            member_name := NULL;
            member_active := NULL;

            IF member_id IS NOT NULL THEN
                SELECT membership.display_name, membership.active
                INTO organization_name, membership_active
                FROM vault.organization_members AS membership
                WHERE membership.organization_id = tenant AND membership.user_id = roster.member_id;
                PERFORM set_config('app.current_user_id', member_id::text, true);
                SELECT identity.display_name INTO member_name
                FROM vault.users AS identity WHERE identity.id = roster.member_id;
                PERFORM set_config('app.current_user_id', actor::text, true);
                member_active := coalesce(membership_active, false) AND member_name IS NOT NULL;
                member_name := coalesce(nullif(organization_name, ''), member_name);
            END IF;
            RETURN NEXT;
        END LOOP;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
    END
    $$;

CREATE FUNCTION vault.manager_rename_organization(tenant uuid, new_name text)
    RETURNS boolean
    LANGUAGE plpgsql SECURITY DEFINER
    SET search_path = pg_catalog
    AS $$
    DECLARE
        previous_tenant text := coalesce(current_setting('app.current_organization_id', true), '');
        previous_actor text := coalesce(current_setting('app.current_user_id', true), '');
        changed boolean;
    BEGIN
        IF NOT has_function_privilege(session_user, 'vault.manager_set_member_active(uuid,uuid,boolean)', 'EXECUTE') THEN
            RAISE EXCEPTION 'Organization administration permission required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        PERFORM vault.bind_manager_context(tenant);
        IF NOT coalesce(vault.is_organization_admin(tenant), false) THEN
            RAISE EXCEPTION 'Organization administrator required' USING ERRCODE = 'insufficient_privilege';
        END IF;
        IF new_name IS NULL OR char_length(btrim(new_name)) NOT BETWEEN 1 AND 200 THEN
            RAISE EXCEPTION 'Enter an organization name of 1 to 200 characters'
                USING ERRCODE = 'invalid_parameter_value';
        END IF;

        UPDATE vault.organizations AS organization SET name = btrim(new_name)
        WHERE organization.id = tenant AND organization.name IS DISTINCT FROM btrim(new_name);
        changed := FOUND;
        IF changed THEN
            INSERT INTO vault.audit_events (organization_id, actor_id, operation, details)
            VALUES (tenant, vault.context_id('app.current_user_id'), 'organization.renamed',
                    jsonb_build_object('name', btrim(new_name)));
        END IF;

        PERFORM set_config('app.current_organization_id', previous_tenant, true);
        PERFORM set_config('app.current_user_id', previous_actor, true);
        RETURN changed;
    END
    $$;

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY manage_manager_principals ON vault.manager_principals
         FOR ALL TO %I USING (true) WITH CHECK (true)', current_user);
END
$migration$;

DO $migration$
DECLARE
    relation text;
BEGIN
    FOREACH relation IN ARRAY ARRAY['organizations', 'users', 'organization_members']
    LOOP
        EXECUTE format(
            'CREATE POLICY provision_identity ON vault.%I FOR INSERT TO %I WITH CHECK (true)',
            relation, current_user);
    END LOOP;
END
$migration$;

DO $migration$
DECLARE
    relation text;
BEGIN
    FOREACH relation IN ARRAY ARRAY['groups', 'group_members']
    LOOP
        EXECUTE format(
            'CREATE POLICY administer_groups ON vault.%I FOR ALL TO %I
             USING (vault.is_organization_admin(organization_id))
             WITH CHECK (vault.is_organization_admin(organization_id))',
            relation, current_user);
    END LOOP;
    EXECUTE format(
        'CREATE POLICY append_group_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation IN (''group.created'', ''group.member_added'', ''group.member_removed'',
                               ''group.renamed'')
         )', current_user);
END
$migration$;

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY append_member_enrollment_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''organization.member_enrolled''
         )', current_user);
END
$migration$;

DO $migration$
DECLARE
    membership_scope text := '
        organization_id = vault.context_id(''app.current_organization_id'')
        AND role IN (''member'', ''admin'')
        AND user_id <> vault.context_id(''app.current_user_id'')';
BEGIN
    -- The schema owner uses these policies only after the operation checks
    -- the authenticated actor's own active administrator membership.
    EXECUTE format(
        'CREATE POLICY inspect_members_for_status ON vault.organization_members
         FOR SELECT TO %I USING (%s)', current_user, membership_scope);
    EXECUTE format(
        'CREATE POLICY change_member_status ON vault.organization_members
         FOR UPDATE TO %I USING (%s) WITH CHECK (%s)',
        current_user, membership_scope, membership_scope);
    EXECUTE format(
        'CREATE POLICY append_member_status_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation IN (''organization.member_deactivated'', ''organization.member_reactivated'')
         )', current_user);
END
$migration$;

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY append_member_role_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''organization.member_role_changed''
         )', current_user);
END
$migration$;

DO $migration$
DECLARE
    transfer_scope text := '
        organization_id = vault.context_id(''app.current_organization_id'')
        AND vault.context_id(''app.ownership_transfer_target'') IS NOT NULL
        AND user_id IN (vault.context_id(''app.current_user_id''),
                        vault.context_id(''app.ownership_transfer_target''))';
BEGIN
    EXECUTE format(
        'CREATE POLICY inspect_ownership_transfer ON vault.organization_members
         FOR SELECT TO %I USING (%s AND user_id <> vault.context_id(''app.current_user_id''))', current_user, transfer_scope);
    EXECUTE format(
        'CREATE POLICY change_ownership_transfer ON vault.organization_members
         FOR UPDATE TO %I USING (%s) WITH CHECK (%s)', current_user, transfer_scope, transfer_scope);
    EXECUTE format(
        'CREATE POLICY append_ownership_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''organization.ownership_transferred''
         )', current_user);
END
$migration$;

DO $migration$
BEGIN
    EXECUTE format(
        'CREATE POLICY read_own_organization_directory ON vault.organization_members FOR SELECT TO %I
         USING (
             current_setting(''app.current_organization_id'', true) = ''''
             AND active AND user_id = vault.context_id(''app.current_user_id'')
             AND EXISTS (SELECT FROM vault.users WHERE id = user_id)
             AND EXISTS (
                 SELECT FROM vault.manager_principals AS principal JOIN pg_catalog.pg_roles AS login
                   ON (login.oid, login.rolname) = (principal.database_role_id, principal.database_role_name)
                 WHERE principal.user_id = organization_members.user_id AND principal.active
                   AND login.rolname = session_user AND login.rolcanlogin
             )
         )', current_user);
    EXECUTE format(
        'CREATE POLICY append_member_name_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''organization.member_name_changed''
         )', current_user);
    EXECUTE format(
        'CREATE POLICY rename_organization ON vault.organizations FOR UPDATE TO %I
         USING (vault.is_organization_admin(id)) WITH CHECK (vault.is_organization_admin(id))', current_user);
    EXECUTE format(
        'CREATE POLICY append_organization_name_audit ON vault.audit_events FOR INSERT TO %I
         WITH CHECK (
             organization_id = vault.context_id(''app.current_organization_id'')
             AND actor_id = vault.context_id(''app.current_user_id'')
             AND operation = ''organization.renamed''
         )', current_user);
END
$migration$;
