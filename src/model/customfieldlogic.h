// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "scheduleio_export.h"
#include "model/customfield.h"
#include <QString>
#include <QStringList>

namespace schedule {
class Project;
class Task;

// Microsoft Project custom field formulas: evaluation, validation and the
// translation between the text a person types ([Cost] * 2, [Risk] = "High")
// and the text Project stores, where every field reference is spelled by its
// field id ([MSPJ188743685] * 2).
//
// The language is Project's (VBA expression) syntax: arithmetic + - * / \ Mod ^,
// string concatenation &, comparisons = <> < > <= >=, Not/And/Or/Xor, string,
// number, True/False and #date# literals, and the usual functions (IIf, Switch,
// Choose, Left/Mid/Right, InStr, Format, DateDiff/DateAdd, ProjDateDiff, ...).
// Field references name a task field as Project spells it ([% Complete],
// [Resource Names]), a custom field by alias or by its own name ([Text1]),
// or a field id ([MSPJ188743685]). Values follow Project: durations and work in
// minutes, % fields 0..100, a flag is True (-1) or False.
class SCHEDULEIO_EXPORT CustomFieldLogic
{
public:
    // Evaluate `formula` for `task`. On a parse or runtime error (unknown field,
    // division by zero, bad argument) returns an invalid QVariant and sets *error.
    static QVariant evaluate(const Project &project, const Task &task,
                             const QString &formula, QString *error = nullptr);

    // Empty when `formula` parses and every field it names exists; otherwise
    // the message to show (it is not evaluated, so data-dependent errors such
    // as a division by zero are not reported).
    static QString validate(const Project &project, const QString &formula);

    // Give every task the project's formula fields (customFieldDefinitions with
    // a formula) and recompute their values, then the summary rows by each
    // field's summaryCalc: none (a formula field's summary has no value), a
    // rollup of the subtasks, or the formula. A failing formula leaves the
    // task's value empty.
    static void recalculate(Project &project);

    // Project's stored spelling, field references as [MSPJ<field id>]. Names
    // that resolve to no field are left as written.
    static QString toProjectFormula(const Project &project, const QString &formula);
    // The readable spelling of a formula Project stored: [MSPJ<id>] becomes the
    // field's name (a custom field's alias where it has one).
    static QString fromProjectFormula(const Project &project, const QString &formula);

    // The full task field id (0x0B40xxxx) a name means: a Project task field,
    // a custom field alias or slot name ("Text1"), or "MSPJ<id>"; 0 if none.
    static quint32 taskFieldId(const Project &project, const QString &name);
    // The custom-field slot name ("Text1", "Number3") for a full field id.
    static QString slotName(quint32 fieldId);
    // The Project task field names formulas may use, for pickers and help.
    static QStringList taskFieldNames();

    static bool acceptsLookupValue(const CustomField &field, const QVariant &value);
    static QString indicatorFor(const CustomField &field);
    // As above, resolving a rule that compares with another field ([Finish])
    // on `task`.
    static QString indicatorFor(const Project &project, const Task &task, const CustomField &field);
};
}
