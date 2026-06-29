# MppCalendar

A working-time calendar. Value type; copyable and equality-comparable.

```cpp
#include "src/model/mppcalendar.h"
```

## Members

| Member | Type | Description |
| :--- | :--- | :--- |
| `uniqueId` | `int` | Stable identity. |
| `name` | `QString` | Calendar name. For a base calendar this is its own name (e.g. *Standard*); for a resource calendar it is the linked resource's name. |
| `baseCalendarUniqueId` | `int` | Unique id of the base calendar this one derives from, or `-1` if this is itself a base calendar. |
| `workingDayMask` | `quint8` | Bitmask of working days. |
| `workingTimes` | `QList<QList<MppTimeRange>>` | Working-time periods per weekday: 7 entries, index 0 = Monday … 6 = Sunday (empty entry = a non-working day). |
| `exceptions` | `QList<MppCalendarException>` | Date-range overrides of the normal week (holidays, one-off working days). |

## Working-day mask

`workingDayMask` packs the seven weekdays into one byte, **Monday = bit 0 … Sunday = bit 6**:

| Day | Bit | Value |
| :--- | :--- | :--- |
| Monday | 0 | `0x01` |
| Tuesday | 1 | `0x02` |
| Wednesday | 2 | `0x04` |
| Thursday | 3 | `0x08` |
| Friday | 4 | `0x10` |
| Saturday | 5 | `0x20` |
| Sunday | 6 | `0x40` |

A standard Monday–Friday calendar is therefore `0x1F`.

```cpp
auto isWorking = [](const MppCalendar &c, int mondayBasedDay) {
    return (c.workingDayMask >> mondayBasedDay) & 1;
};

for (const MppCalendar &c : project.calendars) {
    QString days;
    for (int d = 0; d < 7; ++d) days += isWorking(c, d) ? "W" : "-";
    qInfo().noquote() << c.name << days
                      << (c.baseCalendarUniqueId < 0 ? "(base)" : "(derived)");
}
```

## Working hours

`workingTimes` holds the periods worked on each weekday. `MppTimeRange` is a single period:

| Member | Type | Description |
| :--- | :--- | :--- |
| `start` | `QTime` | Start of the working period. |
| `end` | `QTime` | End of the working period. |

```cpp
static const char *kDays[7] = { "Mon","Tue","Wed","Thu","Fri","Sat","Sun" };
for (int d = 0; d < cal.workingTimes.size(); ++d)
    for (const MppTimeRange &p : cal.workingTimes.at(d))
        qInfo() << kDays[d] << p.start.toString("HH:mm") << "-" << p.end.toString("HH:mm");
```

## Exceptions

`MppCalendarException` is a date-range override (a holiday, or a one-off working day):

| Member | Type | Description |
| :--- | :--- | :--- |
| `fromDate` | `QDate` | First day the exception applies. |
| `toDate` | `QDate` | Last day the exception applies. |
| `name` | `QString` | Exception name (e.g. *Company Holiday*). |
| `working` | `bool` | `true` if the days are working with `workingTimes`; `false` for a day off. |
| `workingTimes` | `QList<MppTimeRange>` | The special working periods when `working` is `true`. |

## Notes

* A **derived** (e.g. resource) calendar leaves a weekday's `workingTimes` empty when that day simply
  inherits the base calendar — resolve it via `baseCalendarUniqueId`. A **base** calendar fills each
  working day, defaulting to 08:00–12:00 and 13:00–17:00 when the file stores no explicit hours.
