// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "model/customfieldlogic.h"
#include "model/project.h"
#include "model/task.h"

#include <QtMath>
#include <functional>

namespace schedule {
namespace {
class Parser {
public:
    Parser(const Project &p, const Task &t, const QString &text)
        : project(p), task(t), source(text.startsWith('=') ? text.mid(1) : text) {}
    QVariant run(QString *error) {
        QVariant value = comparison(); skip();
        if (pos != source.size()) fail(QStringLiteral("Unexpected text at position %1").arg(pos + 1));
        if (error) *error = message;
        return message.isEmpty() ? value : QVariant();
    }
private:
    void skip() { while (pos < source.size() && source.at(pos).isSpace()) ++pos; }
    bool take(QChar c) { skip(); if (pos < source.size() && source.at(pos) == c) { ++pos; return true; } return false; }
    bool take(const QString &s) { skip(); if (source.mid(pos, s.size()).compare(s, Qt::CaseInsensitive) == 0) { pos += s.size(); return true; } return false; }
    void fail(const QString &s) { if (message.isEmpty()) message = s; }
    static bool truth(const QVariant &v) { return v.typeId() == QMetaType::Bool ? v.toBool() : v.toDouble() != 0.0; }
    QVariant comparison() {
        QVariant left = expression(); skip();
        const QStringList ops = {QStringLiteral(">="), QStringLiteral("<="), QStringLiteral("<>"), QStringLiteral("="), QStringLiteral(">"), QStringLiteral("<")};
        for (const QString &op : ops) if (take(op)) {
            const QVariant right = expression();
            const bool numeric = left.canConvert<double>() && right.canConvert<double>();
            const int cmp = numeric ? (left.toDouble() < right.toDouble() ? -1 : left.toDouble() > right.toDouble() ? 1 : 0)
                                    : QString::compare(left.toString(), right.toString(), Qt::CaseInsensitive);
            if (op == "=") return cmp == 0; if (op == "<>") return cmp != 0;
            if (op == ">") return cmp > 0; if (op == "<") return cmp < 0;
            if (op == ">=") return cmp >= 0; return cmp <= 0;
        }
        return left;
    }
    QVariant expression() {
        QVariant left = term();
        for (;;) { if (take('+')) left = left.toDouble() + term().toDouble();
            else if (take('-')) left = left.toDouble() - term().toDouble(); else return left; }
    }
    QVariant term() {
        QVariant left = unary();
        for (;;) { if (take('*')) left = left.toDouble() * unary().toDouble();
            else if (take('/')) { const double d = unary().toDouble(); if (qFuzzyIsNull(d)) { fail("Division by zero"); return {}; } left = left.toDouble() / d; }
            else return left; }
    }
    QVariant unary() { if (take('-')) return -unary().toDouble(); if (take('+')) return unary(); return primary(); }
    QVariant field(const QString &name) {
        const QString key = name.trimmed();
        if (key.compare("Cost", Qt::CaseInsensitive) == 0) return task.cost;
        if (key.compare("Actual Cost", Qt::CaseInsensitive) == 0) return task.actualCost;
        if (key.compare("Remaining Cost", Qt::CaseInsensitive) == 0) return task.remainingCost;
        if (key.compare("Work", Qt::CaseInsensitive) == 0) return double(task.workMillis) / 60000.0;
        if (key.compare("Duration", Qt::CaseInsensitive) == 0) return double(task.durationMillis) / 60000.0;
        if (key.compare("Percent Complete", Qt::CaseInsensitive) == 0) return task.percentComplete * 100.0;
        if (key.compare("Priority", Qt::CaseInsensitive) == 0) return task.priority;
        if (key.compare("Total Slack", Qt::CaseInsensitive) == 0) return double(task.totalSlackMillis) / 60000.0;
        for (const CustomField &f : task.customFields) if (f.name.compare(key, Qt::CaseInsensitive) == 0) return f.value;
        fail(QStringLiteral("Unknown field [%1]").arg(key)); return {};
    }
    QVariant primary() {
        skip();
        if (take('(')) { QVariant v = comparison(); if (!take(')')) fail("Missing )"); return v; }
        if (take('[')) { const int end = source.indexOf(']', pos); if (end < 0) { fail("Missing ]"); return {}; } const QString n = source.mid(pos, end-pos); pos=end+1; return field(n); }
        if (pos < source.size() && (source.at(pos) == '\'' || source.at(pos) == '"')) { const QChar q=source.at(pos++); const int end=source.indexOf(q,pos); if(end<0){fail("Unterminated string");return{};} QString s=source.mid(pos,end-pos);pos=end+1;return s; }
        int begin=pos; while(pos<source.size() && (source.at(pos).isLetterOrNumber() || source.at(pos)=='.' || source.at(pos)=='_')) ++pos;
        const QString word=source.mid(begin,pos-begin); bool ok=false; double number=word.toDouble(&ok); if(ok)return number;
        if (word.compare("True",Qt::CaseInsensitive)==0) return true; if(word.compare("False",Qt::CaseInsensitive)==0)return false;
        if (word.compare("IIf",Qt::CaseInsensitive)==0 && take('(')) { QVariant c=comparison(); if(!take(','))fail("IIf needs commas"); QVariant yes=comparison(); if(!take(','))fail("IIf needs commas"); QVariant no=comparison(); if(!take(')'))fail("Missing )"); return truth(c)?yes:no; }
        if (word.compare("Round",Qt::CaseInsensitive)==0 && take('(')) { double v=comparison().toDouble(); int digits=0; if(take(','))digits=comparison().toInt(); if(!take(')'))fail("Missing )"); const double p=qPow(10.0,digits);return qRound64(v*p)/p; }
        fail(QStringLiteral("Expected a value at position %1").arg(begin+1)); return {};
    }
    const Project &project; const Task &task; QString source; int pos=0; QString message;
};

bool compareRule(const QVariant &left, const CustomField::IndicatorRule &rule)
{
    const QString op = rule.comparison.toLower();
    if (op == "contains") return left.toString().contains(rule.value.toString(), Qt::CaseInsensitive);
    const double a=left.toDouble(), b=rule.value.toDouble();
    if(op=="eq")return left==rule.value; if(op=="ne")return left!=rule.value;
    if(op=="lt")return a<b; if(op=="le")return a<=b; if(op=="gt")return a>b; if(op=="ge")return a>=b;
    return false;
}
}

QVariant CustomFieldLogic::evaluate(const Project &project, const Task &task, const QString &formula, QString *error)
{ return Parser(project, task, formula).run(error); }

void CustomFieldLogic::recalculate(Project &project)
{
    for (Task &task : project.tasks)
        for (int pass=0; pass<task.customFields.size(); ++pass)
            for (CustomField &field : task.customFields)
                if (!field.formula.trimmed().isEmpty()) { QString error; QVariant v=evaluate(project,task,field.formula,&error); if(error.isEmpty()) field.value=v; }
}

bool CustomFieldLogic::acceptsLookupValue(const CustomField &field, const QVariant &value)
{ return field.lookupValues.isEmpty() || field.lookupValues.contains(value.toString(), Qt::CaseInsensitive); }

QString CustomFieldLogic::indicatorFor(const CustomField &field)
{ for (const auto &rule : field.graphicalIndicators) if (compareRule(field.value,rule)) return rule.indicator; return {}; }
}
