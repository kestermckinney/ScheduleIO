# Project Reconciliation

Include `src/model/projectreconciliation.h` when assignment time-phased work or cost inputs change.

```cpp
schedule::ProjectReconciliation::reconcile(project);

const auto task = schedule::ProjectReconciliation::taskTotals(project, taskUid);
const auto resource = schedule::ProjectReconciliation::resourceTotals(project, resourceUid);
const QStringList issues =
    schedule::ProjectReconciliation::invariantViolations(project);
```

## Authority rules

- Actual Work buckets are authoritative for assignment Actual Work when present.
- Remaining Work buckets are authoritative for assignment Remaining Work when present.
- A stream without buckets keeps its aggregate value. An explicit nonzero Remaining Work is treated
  as the estimate to complete; otherwise it is derived as Work minus Actual Work.
- Assignment Work always reconciles to Actual Work plus Remaining Work.
- Only Work-resource assignments roll into task labor. Material and Cost resources contribute cost,
  but not task work or duration.
- Task fixed cost is included in task cost. Until the task is complete it is classified as remaining;
  after completion it is classified as actual. Fixed-cost accrual modes are not modeled yet.
- Work-resource rates use the assignment's selected A-E table and the rate effective at each bucket's
  start. Cost-per-use is applied once, to actual cost after work starts or otherwise to remaining cost.
- Imported aggregate costs remain authoritative when no applicable modeled rate exists.
- Summary tasks roll up active leaf tasks; inactive tasks do not contribute to task or resource totals.

`invariantViolations()` is non-mutating. Call it after reconciliation in tests or before a guarded
save. It reports stale assignment buckets, assignment Work/Cost equations, task rollups, and resource
cost rollups. Overtime, time-phased cost streams, baseline buckets, and cost-resource entry are
separate feature areas and are not synthesized by this API.
