# MppCustomField

One custom ("extended") field value on a task, resource, or assignment. Value type; copyable and
equality-comparable.

```cpp
#include "src/model/mppcustomfield.h"
```

Microsoft Project exposes a large, fixed set of custom slots per entity — `Text1`–`Text30`,
`Number1`–`Number20`, `Cost1`–`Cost10`, `Date1`–`Date10`, `Duration1`–`Duration10`,
`Start1`–`Start10`, `Finish1`–`Finish10`, `Flag1`–`Flag20` and `Outline Code1`–`Outline Code10`.
Rather than ~150 explicit members, MppIO returns only the slots that are actually populated as a
`QList<MppCustomField>` on each [`MppTask`](MppTask.md), [`MppResource`](MppResource.md) and
[`MppAssignment`](MppAssignment.md).

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `fieldId` | `int` | The full Microsoft field id (entity high word \| field index). Equals the `<FieldID>` in a Microsoft Project XML export. |
| `name` | `QString` | Human label, e.g. `"Text1"`, `"Cost2"`, `"Outline Code1"`. |
| `value` | `QVariant` | The decoded value in its natural Qt type. |

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

## Example

```cpp
for (const MppTask &t : project.tasks) {
    for (const MppCustomField &c : t.customFields) {
        if (c.name == QLatin1String("Text1"))
            qInfo() << t.name << "Text1 =" << c.value.toString();
        else
            qInfo() << t.name << c.name << "=" << c.value;
    }
}
```
