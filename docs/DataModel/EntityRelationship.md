# Entity Relationship Diagram

The diagram maps the complete public ScheduleIO project model: its core entities, every public
attribute, collection-owned value types, and the identity links between records. It is an SVG, so
the artwork stays sharp when enlarged. Scroll the embedded view horizontally, or open the SVG at
full size to zoom in your browser.

<div class="erd-frame">
  <p><a href="../../assets/scheduleio-erd.svg" target="_blank" rel="noopener">Open the full-size SVG</a></p>
  <div class="erd-scroll">
    <img src="../../assets/scheduleio-erd.svg" alt="ScheduleIO ERD showing every model entity and attribute, primary keys, foreign keys, and relationships." width="2000" height="7900">
  </div>
</div>

## Identity and relationships

The C++ model is a collection of value types rather than a database schema. `uniqueId` is the
stable identity on Task, Resource, Assignment, Relation, and Calendar. The Project is the root
document and has no standalone UID. `id` on Task and Resource is a display position, not a key.

| Relationship | Link |
| :--- | :--- |
| Project → Task, Resource, Assignment, Relation, Calendar, custom field definitions | Project-owned collections |
| Task → Assignment | `Assignment.taskUniqueId` → `Task.uniqueId` |
| Resource → Assignment | `Assignment.resourceUniqueId` → `Resource.uniqueId` |
| Task → Relation | `Relation.predecessorTaskUid` and `successorTaskUid` → `Task.uniqueId` |
| Project, Task, Resource → Calendar | `calendarUniqueId` → `Calendar.uniqueId` |
| Calendar → Calendar | `baseCalendarUniqueId` → `Calendar.uniqueId` |
| Task, Resource, Assignment → Baseline and CustomField | Owner-scoped value collections |
| Resource → CostRate and AvailabilityPeriod | Resource-owned value collections |
| Assignment → TimephasedValue | Assignment-owned value collection |
| Calendar → CalendarException and TimeRange | Calendar-owned working-time collections |
| Task → TaskSegment and EarnedValue | Task-owned split segments and earned-value record |

Collection-owned values do not have independent IDs in the C++ types. The SVG uses PK badges for
stable entity IDs and PK* badges for conceptual owner-scoped or composite keys; FK badges mark
explicit UID references. Task hierarchy is represented by task order, `outlineLevel`, and WBS;
there is no parent-task foreign key.
