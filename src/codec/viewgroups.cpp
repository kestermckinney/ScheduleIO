// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// The group a view has applied: Props9 item 0x22400018 holds its name and
// 0x2240001D an 83-byte copy of its definition. The copies below are the bytes
// Project 2016 wrote for each group (GroupApply on the Gantt Chart, Task Usage
// and Resource Usage views; samples in tests/fixtures/mpp_samples/viewgroup).
// Layout, as far as it varies: u16@0 1 = no group / 3 = grouped, u8@5 1 for
// the Status group, u32@12 the grouping field id (0x0B40xxxx task,
// 0x0C40xxxx resource), u8@16 1 for a task field, u8@51 the interval
// (2 = Project's 0% / 1-99% / 100% percent bands), and the group header's
// cell colour at @34 (0xC1F6FF).

#include "model/usageviewsettings.h"

namespace schedule {

QByteArray projectGroupDefinition(const QString &name)
{
    static const struct { const char *name; const char *hex; } kGroups[] = {
        { "No Group",
          "0100000000000000000100000000000000000000000000000000000000000000000000000000000000"
          "000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
        { "Critical",
          "0300000000000000000001001300400b010000000001000000000000000000000000fff6c100000000"
          "000000000081000000000000000000000000000000000000000000000000000000000000000000000000" },
        { "Status",
          "0300000000010000000001001f02400b010000000001000000000000000000000000fff6c100000000"
          "000000000081000000000000000000000000000000000000000000000000000000000000000000000000" },
        { "Complete and Incomplete Tasks",
          "0300000000000000000001002000400b010000000001000000000000000000000000fff6c100000000"
          "000000000081000000020000000000000000000000000000000000000000000000000000000000000000" },
        { "Auto Scheduled v. Manually Scheduled",
          "0300000000000000000001000005400b010000000001000000000000000000000000fff6c100000000"
          "000000000081000000000000000000000000000000000000000000000000000000000000000000000000" },
        { "Auto Scheduled vs. Manually Scheduled",
          "0300000000000000000001000005400b010000000001000000000000000000000000fff6c100000000"
          "000000000081000000000000000000000000000000000000000000000000000000000000000000000000" },
        { "Outline Level",
          "0300000000000000000001005500400b010000000001000000000000000000000000fff6c100000000"
          "000000000081000000000000000000000000000000000000000000000000000000000000000000000000" },
        { "Resource Type",
          "0300000000000000000001002c01400c000000000001000000000000000000000000fff6c100000000"
          "000000000081000000000000000000000000000000000000000000000000000000000000000000000000" },
    };
    QString bare = name;
    bare.remove(QLatin1Char('&'));
    for (const auto &g : kGroups)
        if (bare == QLatin1String(g.name))
            return QByteArray::fromHex(g.hex);
    return {};
}

bool UsageViewSettings::setGroup(const QString &name)
{
    const QByteArray definition = projectGroupDefinition(name);
    if (definition.isEmpty())
        return false;
    groupName = name;
    groupDefinition = definition;
    present = true;
    return true;
}

} // namespace schedule
