-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

CREATE SCHEMA IF NOT EXISTS vault_admin;

CREATE TABLE vault_admin.desktop_capability_events (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    recorded_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    operator_login name NOT NULL,
    operator_role name NOT NULL,
    target_login name NOT NULL,
    capability text NOT NULL,
    action text NOT NULL CHECK (action IN ('enabled', 'revoked'))
);

REVOKE ALL ON vault_admin.desktop_capability_events FROM PUBLIC;

ALTER FUNCTION vault.enable_desktop_capability(name, text) RENAME TO grant_desktop_capability_privileges;
ALTER FUNCTION vault.revoke_desktop_schedule_updates(text) RENAME TO revoke_desktop_schedule_update_privileges;

CREATE FUNCTION vault.enable_desktop_capability(desktop_login name, capability text)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        PERFORM vault.grant_desktop_capability_privileges(desktop_login, capability);
        INSERT INTO vault_admin.desktop_capability_events
            (operator_login, operator_role, target_login, capability, action)
        VALUES (session_user, current_user, desktop_login, capability, 'enabled');
    END
    $$;

CREATE FUNCTION vault.revoke_desktop_schedule_updates(desktop_login text)
    RETURNS void
    LANGUAGE plpgsql
    SET search_path = pg_catalog
    AS $$
    BEGIN
        PERFORM vault.revoke_desktop_schedule_update_privileges(desktop_login);
        INSERT INTO vault_admin.desktop_capability_events
            (operator_login, operator_role, target_login, capability, action)
        VALUES (session_user, current_user, desktop_login::name, 'schedule_updates', 'revoked');
    END
    $$;

REVOKE ALL ON FUNCTION vault.enable_desktop_capability(name, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION vault.revoke_desktop_schedule_updates(text) FROM PUBLIC;
