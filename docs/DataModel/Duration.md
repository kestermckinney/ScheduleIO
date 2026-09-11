# Duration and Unit Conventions

ScheduleIO stores duration, work, lag, delay, and slack as milliseconds. A separate Microsoft Project
unit code controls parsing and display; it does not change the canonical stored quantity.

```cpp
#include "model/duration.h"
```

## schedule::Duration::Unit

| Unit | Code | Basis |
| :--- | :--- | :--- |
| `Minutes` | `3` | 60 seconds |
| `ElapsedMinutes` | `4` | 60 seconds, ignores calendars |
| `Hours` | `5` | 60 minutes |
| `ElapsedHours` | `6` | 60 minutes, ignores calendars |
| `Days` | `7` | Active working-time profile, normally 8 hours |
| `ElapsedDays` | `8` | 24 hours |
| `Weeks` | `9` | Active profile, normally 5 working days |
| `ElapsedWeeks` | `10` | 7 elapsed days |
| `Months` | `11` | Active profile, normally 20 working days |
| `ElapsedMonths` | `12` | 30 elapsed days |

Estimated Microsoft unit codes are the base code plus 32 (for example `39` for estimated days).
`normalizeUnit()` maps them to the base range. Null/unknown codes normalize to Days.

## schedule::Duration::WorkingTimeProfile

| Member | Type | Default |
| :--- | :--- | :--- |
| `millisPerDay` | `qint64` | 8 hours |
| `millisPerWeek` | `qint64` | 40 hours |
| `millisPerMonth` | `qint64` | 160 hours |

`Project::minutesPerDay`, `minutesPerWeek`, and `daysPerMonth` persist a document's settings.
`setWorkingTimeProfile()` activates those values for process-global duration conversion;
`resetWorkingTimeProfile()` restores Microsoft defaults, and `workingTimeProfile()` returns the
active values. Because the profile is process-global, applications displaying multiple projects
concurrently should set it deliberately around conversions.

Elapsed units never use the working-time profile or calendar. Scheduling with a non-elapsed duration
uses `WorkCalendar` to skip non-working periods.

## Functions

| Function | Purpose |
| :--- | :--- |
| `isElapsed(unit)` | Reports whether the normalized unit ignores working calendars. |
| `unitMillis(unit)` | Milliseconds represented by one unit under the active profile. |
| `normalizeUnit(unit)` | Removes the estimated flag and handles unknown/null codes. |
| `toMillis(value, unit)` | Converts a numeric duration to canonical milliseconds. |
| `fromMillis(millis, unit)` | Converts milliseconds to a numeric display value. |
| `format(millis, unit)` | Produces Project-like text such as `3 days`, `2.5 wks`, or `4 hrs`. |
| `parse(text, &millis, &unit, defaultUnit)` | Parses signed input such as `3d`, `1.5 weeks`, `30 min`, or `2 emo`. |

`parse()` returns `false` without changing its outputs when input is invalid. A bare number uses the
supplied default unit, which defaults to Days.

```cpp
qint64 millis = 0;
int unit = schedule::Duration::Days;
if (schedule::Duration::parse("2.5d", &millis, &unit))
    qInfo() << millis << schedule::Duration::format(millis, unit);
```
