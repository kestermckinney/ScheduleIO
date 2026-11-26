-- Copyright (C) 2026 Paul McKinney
-- SPDX-License-Identifier: GPL-3.0-only

-- Full-model reads contain unmapped resources and native presentation data.
CREATE POLICY read_full_schedule_data ON vault.schedule_data FOR SELECT
    USING (vault.has_project_role(organization_id, project_id, ARRAY['manager']));

CREATE POLICY read_live_resources_for_managers ON vault.resources FOR SELECT
    USING (NOT retired AND vault.has_project_role(organization_id, project_id, ARRAY['manager']));
