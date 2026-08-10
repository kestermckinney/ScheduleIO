// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "scheduleio_export.h"
#include "model/customfield.h"
#include <QString>

namespace schedule {
class Project;
class Task;

class SCHEDULEIO_EXPORT CustomFieldLogic
{
public:
    static QVariant evaluate(const Project &project, const Task &task,
                             const QString &formula, QString *error = nullptr);
    static void recalculate(Project &project);
    static bool acceptsLookupValue(const CustomField &field, const QVariant &value);
    static QString indicatorFor(const CustomField &field);
};
}
