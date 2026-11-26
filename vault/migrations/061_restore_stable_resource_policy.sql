-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- pg_dump deparses a row comparison as nested AND expressions. Use the
-- equivalent flat form so restoring the policy preserves its catalog digest.
ALTER POLICY read_own_mapped_live_resources ON vault.resources
    USING (
        NOT retired AND EXISTS (
            SELECT FROM vault.resource_user_mappings AS mapping
            WHERE mapping.organization_id = resources.organization_id
              AND mapping.project_id = resources.project_id
              AND mapping.resource_id = resources.id
              AND mapping.user_id = vault.context_id('app.current_user_id')
              AND mapping.effective_from <= transaction_timestamp()
              AND mapping.effective_until IS NULL
        )
    );
