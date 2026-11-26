# ScheduleIO Vault Data Model

A Vault is a PostgreSQL database that stores one live ScheduleIO schedule per project and the organization data needed to share schedules, align resources, and record time. Local and remote Vaults use the same schema, migrations, and permission rules. Schedule Vault Manager, a separate private application, provisions and administers a Vault. ScheduleVault is the application people use to work with schedules and time.

## Storage and keys

Use a shared schema within each Vault. Every tenant-owned table has a non-null `organization_id`. Use UUIDs for Vault identities, with composite keys and foreign keys that carry the full scope of the relationship. For example, an assignment references its task by `(organization_id, project_id, task_id)`. Sharing and membership foreign keys include the organization, and time entries use the full project scope.

```text
organizations
├── organization_members ─────────────── users
├── groups
│   └── group_members ────────────────── organization_members
└── projects
    ├── schedule_data                    one live model payload
    ├── project_import_sources           source keys and file digests
    ├── project_members ──────────────── organization_members
    ├── project_group_shares ─────────── groups
    ├── tasks ────────────────────────── parent task, when present
    ├── resources
    │   └── resource_user_mappings ───── organization_members
    ├── assignments ─────────────────── task and resource
    ├── time_entries ────────────────── organization member, task,
    │   │                               and optional assignment
    │   └── time_entry_events
    └── transfer_jobs and audit_events  can also be organization-scoped
```

| Tables | Responsibility and constraints |
| --- | --- |
| `organizations`, `users`, `organization_members` | Organizations are tenant roots. Users are global within a Vault and identified by a stable authentication subject. Membership is unique by organization and user, with owner, admin, or member role and an active state. Tenant-facing user lookups go through authorized membership views. |
| `groups`, `group_members` | Named groups and their members. Every group member references an organization membership in the same tenant. |
| `projects` | A project holds its schedule metadata, lifecycle status, and an edit counter for concurrent saves. An authorized save changes the existing schedule in place. |
| `project_import_sources` | Binds a recognized source identity to a project within an organization. A trusted source identifier, where available, is unique within the tenant and source namespace. |
| `schedule_data` | Holds the project-level metadata and collections for the live schedule. Its format revision identifies the payload encoding. |
| `tasks`, `resources`, `assignments` | Each row has a stable Vault identity within its project. Tasks carry a derived parent link. Assignments reference same-project task and resource rows, with model-supported absent references represented explicitly. A retired row retains its identity for historical references. |
| `resource_user_mappings` | One active organization user per logical project resource, with effective dates and audit history. A user can map to multiple resources. Equipment and material remain schedule resources. Mapping operations apply to eligible work resources. |
| `project_members`, `project_group_shares` | Direct user and group grants. Each grant references a same-tenant organization member or group and a same-tenant project. Roles can be combined. |
| `time_entries`, `time_entry_events` | Time belongs to an organization member and a task identity. The task foreign key targets the retained identity. An optional assignment foreign key targets the retained assignment identity and includes its task key, so it cannot point to a different task. Retain the authorizing mapping or project grant in the audit record. |
| `transfer_jobs`, `audit_events` | Track imports, exports, and administrative changes. Transfer jobs record the actor, project where known, status, timestamps, request identity, and structured diagnostics. Source files and exported artifacts follow the operator's file access and retention policies. |

Archive projects and deactivate memberships without removing their historical identity. Retain retired task and assignment identities referenced by time or audit records. Keep the task and resource endpoints immutable on each assignment identity. If an import moves an assignment to a different task or resource, retire the old identity and create a new one so time stays attributed to the original work. Exclude retired items from live schedules and new time entry. Bind tenant, project, and actor from validated context when creating records. Record security changes and their actor in the same transaction as the change.

## ScheduleIO fidelity

The canonical model is [`schedule::Project`](../DataModel/Overview.md), shared by [MppIO](../API/MppIO.md) and [XmlIO](../API/XmlIO.md). The implementation field checklist comes from the [entity relationship diagram](../DataModel/EntityRelationship.md) and [field coverage](../Reference/FieldCoverage.md). Before shipping the adapter, classify every model field as persisted, derived, transient, or opaque.

The current schema stores model fidelity fields in versioned binary payloads on the project and entity rows. Queryable fields such as task names and dates use separate typed columns or JSONB properties. Task dependencies retain their ScheduleIO endpoints, type, and lag in the model payload. A relational dependency table would support database-side traversal if that becomes necessary.

| Model data | Vault representation |
| --- | --- |
| Project properties and ordered entity collections | Store semantic fields on the project and its child rows. Preserve collection order, numeric source UIDs, and display IDs separately from Vault IDs. Enforce source UID uniqueness per entity type and project among live rows where a UID exists. |
| Task outline, WBS, and numbering | Preserve the source fields and ordering. Derive parent IDs from the flat outline for queries and validate parent scope and cycles. |
| Calendars, base links, working periods, exceptions, baselines, rate tables, availability, splits, and time-phased values | Preserve links and ordered values in the versioned ScheduleIO payloads. Add query projections where Vault features need these values directly. |
| Custom field definitions and values, view styles, timeline and usage settings, and raw notes | Preserve owner, type, order, and missing values. Keep raw Rich Text Format notes and model formatting. |
| Native preservation data | Store model-carried payloads such as `mppSourceTemplate`, `mppFontBases`, `mppBarExceptions`, and timeline XML in protected project-scoped storage. Full-project export must check access to that data because embedded fields can contain information beyond visible schedule rows. |

Use UTC timestamps for model instants, date and time columns for calendar-local values, 64-bit milliseconds for work and durations, and ratio values for units and percentages. Translate absent references and invalid date values using per-field rules. Preserve valid zero values and model-supported special rows. Omit transient edit flags. Document when ScheduleIO recalculates derived fields so opening and saving a Vault schedule has predictable effects.

## Open, save, and re-import

Add a C++ Vault adapter in ScheduleIO alongside the file facades. It loads an authorized live schedule into a `schedule::Project` and saves it against the edit counter read when the schedule was opened. After the initial import, only ScheduleVault can edit or re-import the schedule, using ScheduleIO through its authenticated write path. Grant schedule update access only to that path. Import uses the existing MPP or Microsoft Project Data Interchange (MSPDI) XML reader, and export uses the corresponding writer and reports format-specific coverage limits.

1. Resolve the source against known project bindings before creating a project. Use reliable source identifiers where available and use a file digest to recognize identical content. Manager requires the operator to opt in to creating a project for an unmatched file. If the match is ambiguous, ask the operator to select the existing project or confirm a distinct source. Check source bindings across the tenant, including projects the operator cannot open. Reject conflicting creation without disclosing restricted project details. Filename or title can suggest a match. When the source matches an existing project, ScheduleVault previews an update to that project.
2. Compare the incoming data with the live schedule. Match entity UIDs within the confirmed project lineage and require explicit resolution for changed or reused UIDs and conflicts with edits already made in ScheduleVault. Keep project grants, resource mappings on matched identities, and time associations. Preview retirements and require review of affected mappings and time links. An import never grants access through a suggested match.
3. Apply the approved changes, including validated references, mapping changes, edit counter, and audit record, in one transaction. Check the expected edit counter under lock. If another save has changed the project, refresh the preview. Enforce source-binding uniqueness during initial imports and use a unique request key for safe retries after an uncertain commit. A successful desktop repeat import records its receipt in the same transaction as the update and digest binding.
4. Load the live schedule under one consistent database snapshot. A failed update rolls back and records the job failure separately. Keep import diagnostics and audit events for operations review. The edit counter is solely a concurrency check.

Full-project reads and exports require full-schedule permission. Assignment-scoped views use dedicated projections that restrict related tasks, resources, notes, dependencies, and attachments to the caller's access. A filtered export builds a fresh model with valid references and a preview of omitted rows and dependencies. Export preserves UIDs for included entities where supported by the writer.

In ScheduleVault, **File → Shared Vault Tasks** lists visible tasks for a project using the project directory capability. It shows names, visible hierarchy, dates, progress, visible predecessor links, and authorized assignments with planned work. A mapped contributor can see their resource’s name. Schedules imported before these display fields were added gain the fields on their next save or repeat import.

## Database authorization

Enable and force Row Level Security (RLS) on all tenant tables, including job, audit, and schedule payload tables. Run application queries as non-owner roles without superuser or `BYPASSRLS`. Give schema ownership to a separate migration role. Deny access when context is missing, invalid, or tied to an inactive membership.

A trusted service authenticates the user and selects the requested tenant after checking membership. It sets actor and tenant with transaction-local `set_config(..., true)` before the first application query. Treat custom PostgreSQL settings as context carriers because a SQL client with the runtime credential can set them. Keep that credential on the trusted service. Direct desktop sessions bind the authenticated PostgreSQL login to a Vault user. PostgreSQL checks that login's function grants and project permissions. An initial-import-only login cannot update an existing schedule. A login granted schedule updates can also call those functions from another PostgreSQL client, so keep that capability scoped to trusted logins. Manager diagnostics exercise the same database authorization path.

Apply a restrictive tenant and active membership predicate, then command-specific policies for reads and writes. Use `USING` for existing rows and `WITH CHECK` for resulting rows. Keep authorization lookup functions small and avoid policies that query themselves through other protected tables. If a security-definer helper is needed, fix its search path, qualify objects, restrict execution, and define its owner and access under forced RLS. Review both table grants and policy behavior. Use trusted functions for binding time ownership and enforcing approval transitions.

| Role or relationship | Effective access inside the active organization |
| --- | --- |
| Organization owner or admin | Manage tenant memberships and projects, read organization schedules, map resources, and approve project time. Owner-level operations include ownership transfer. |
| Project manager | Read and save the full schedule through ScheduleVault, administer project grants and mappings, view unmapped resources, and approve project time. |
| Editor or viewer | Editor can save schedule changes through ScheduleVault. Viewer can read the shared schedule. Both are subject to resource, cost, and export restrictions. |
| Contributor with a mapped resource | See assigned tasks and own assignments, and log own time against that work. A mapping alone requires a separate project grant for access. |
| Time Entry | Log own time on eligible project tasks or assignments visible to the caller, including task-only entries without a mapped resource. |
| Time Approver | Review and approve submitted project time. Grant schedule visibility separately as needed. |

Combine direct and group permissions by capability. Check active membership and group membership on each request so revocations affect new requests. Unmapped resource details are manager/admin only. Expose sensitive costs or native payloads through separately granted projections or operations. Keep personal time visible to the logger and authorized project approvers/managers and organization admins. Prevent self-approval by default.

## Time and resourcing

ScheduleVault’s **Vault Time** dialog can open its own PostgreSQL connection for users who can log or review time without opening a full schedule. It lists eligible task and assignment targets for the chosen project. Provision the registered desktop login with execution privileges on `vault.desktop_list_time_work`, `vault.desktop_list_time`, `vault.desktop_create_time`, and `vault.desktop_update_time` as its duties require. The project and resource permissions are checked on each call.

Store work date in the organization's configured time zone, positive integer work milliseconds, comments, billable flag, and status. Convert hours at the UI boundary. Drafts can be edited by the logger. Submission freezes the work fields. Approval or return requires the approver role and records actor, timestamp, and outcome. Corrections to approved time create linked adjustments. Add period-based timesheets later if approvals need batch grouping.

Vault Time defaults new entries to the current work date in the organization’s time zone. It shows the submitter and the task name recorded when the entry was created. The name stays with the entry if the task is later renamed or retired. Older entries can show a current task name when the caller can still see that task.

For export, keep imported actuals as the default. The Manager’s MSPDI option replaces imported actual work for assignments with matching approved Vault time. It uses organization work dates, retains imported actuals for other assignments, and reports the count of approved entries without a matching exported assignment. An explicit actuals export builds a copy with approved entries attributed to the correct assignment and reporting date. Preview unresolved task-only entries and overlaps with existing actuals. Require an explicit inclusion rule before combining the two streams to avoid counting work twice. Apply [ScheduleIO reconciliation](../API/Reconciliation.md) after updating time-phased values and report the changes before export.

Organization admins can see organization-wide demand and utilization. Project managers can review their managed projects across the organization, including a group-filtered view for a group they belong to. Organization admins can filter by any group in their organization. Wider portfolio coverage requires an explicit reporting grant. Count each project’s live schedule once, roll labor demand up by mapped user across projects, and show unmapped demand separately to authorized viewers. Report imported actual work and approved Vault time as separate measures by default. Report historical time from retained work records.

## Indexing and verification

Lead tenant-scoped indexes with `organization_id`. Index active membership by user, group membership by user and group, direct and group grants by project and principal, mappings by user and resource, and each commonly traversed composite foreign key. Index task parents and assignment tasks/resources within the project, and time by user/work date and project/status/work date. Start with caller-scoped reporting views and measure query plans under the actual runtime roles. Add materialized summaries or partitioning only when volume warrants it. Any stored summary must carry tenant and access scope.

Acceptance covers cross-tenant and cross-project key rejection, missing and forged context, pooled connection reuse, revoked grants, group and direct grants, denied writes, and time approval. Compare representative MPP and MSPDI fixtures after file → model → Vault → model → file, including absent values, hierarchy, baselines, calendars, custom fields, time-phased values, and opaque payloads. Exercise concurrent saves, repeat imports with stable project and time associations, retries after interrupted imports, replayable exports, and reports that span projects with different grants.

The PostgreSQL regression test `tst_vault_reimport` uses a disposable database. Build with `SCHEDULEIO_BUILD_VAULT=ON` and `SCHEDULEIO_BUILD_TESTS=ON`. Set `SCHEDULEIO_VAULT_TEST_DATABASE` to a fresh database, `SCHEDULEIO_VAULT_TEST_OWNER` to its non-superuser owner, and `SCHEDULEIO_VAULT_TEST_ACTOR` to a separate unprivileged login. Connect with a PostgreSQL test administrator using `PGHOST`, `PGPORT`, `PGUSER`, and `PGPASSWORD`. The test applies migrations, creates fixture data, and switches to the unprivileged login before exercising import and time-entry operations.

Run the database regression with `ctest --test-dir build -L postgresql --output-on-failure`. CTest reports it as skipped when the fixture database is unset. The export-selection tests run independently and need no database.
