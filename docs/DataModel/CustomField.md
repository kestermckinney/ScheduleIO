# schedule::CustomField

One custom ("extended") field value on a task, resource, or assignment. Value type; copyable and
equality-comparable.

```cpp
#include "src/model/customfield.h"
```

Microsoft Project exposes a large, fixed set of custom slots per entity — `Text1`–`Text30`,
`Number1`–`Number20`, `Cost1`–`Cost10`, `Date1`–`Date10`, `Duration1`–`Duration10`,
`Start1`–`Start10`, `Finish1`–`Finish10`, `Flag1`–`Flag20` and `Outline Code1`–`Outline Code10`.
Rather than ~150 explicit members, MppIO returns only the slots that are actually populated as a
`QList<schedule::CustomField>` on each [`schedule::Task`](Task.md), [`schedule::Resource`](Resource.md) and
[`schedule::Assignment`](Assignment.md).

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `fieldId` | `int` | The full Microsoft field id (entity high word \| field index). Equals the `<FieldID>` in a Microsoft Project XML export. |
| `name` | `QString` | Human label, e.g. `"Text1"`, `"Cost2"`, `"Outline Code1"`. |
| `value` | `QVariant` | The decoded value in its natural Qt type. |
| `formula` | `QString` | Formula expression, such as `[Cost] / [Duration]`; usually populated on a project-level definition. |
| `lookupValues` | `QStringList` | Allowed values for a lookup-backed field. |
| `graphicalIndicators` | `QList<IndicatorRule>` | Ordered comparison/value/icon rules. |

## Value types

The `value` holds the natural Qt type for each kind of slot:

| Slot kind | `QVariant` type | Notes |
| :--- | :--- | :--- |
| `Text*`, `Outline Code*` | `QString` | |
| `Number*` | `double` | |
| `Cost*` | `double` | In the project's currency unit. |
| `Date*`, `Start*`, `Finish*` | `QDateTime` | UTC. |
| `Duration*` | `qint64` | Milliseconds. |
| `Flag*` | `bool` | |

## schedule::CustomField::IndicatorRule

| Member | Type | Meaning |
| :--- | :--- | :--- |
| `comparison` | `QString` | Comparison operator: `eq`, `ne`, `lt`, `le`, `gt`, `ge`, or `contains`. |
| `value` | `QVariant` | Right-hand value compared with the field value. |
| `indicator` | `QString` | Symbolic icon name used by the consuming UI/report. |

`Project::customFieldDefinitions` carries definition metadata. A task/resource/assignment
`customFields` list carries the field's value and may also carry metadata after import. Match by
`fieldId` when possible; names are useful for display but can be localized or renamed.

`CustomFieldLogic::evaluate()` supports arithmetic, comparisons, field references, `IIf`, and
`Round` over task values. `recalculate()` evaluates project definitions for tasks,
`acceptsLookupValue()` validates lookup membership, and `indicatorFor()` selects a graphical rule.

## Example

```cpp
for (const schedule::Task &t : project.tasks) {
    for (const schedule::CustomField &c : t.customFields) {
        if (c.name == QLatin1String("Text1"))
            qInfo() << t.name << "Text1 =" << c.value.toString();
        else
            qInfo() << t.name << c.name << "=" << c.value;
    }
}
```
