// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
//
// Custom field formulas and aliases against Microsoft Project 2016. The files
// in tests/fixtures/mpp_samples/customfields were saved by Project on dpr2hw3
// (ScheduleVault/tools/customfield-samples-remote.vbs):
//   cf-eval        25 formula fields; the expected values below are what
//                  Project calculated for each task (its "EVAL" log lines)
//   cf-formula-ref Number1 "Score" = [Cost]*2, Number2 "Double" = [Score]*2
//   cf-formula-text Text2 "Label" = [Name] & " - " & [% Complete]
//   cf-alias       Text1 aliased "Risk", values High / Low
//   cf-kinds       an active formula on one field of every kind
//   cf-base        the same project with no custom fields
//   lk-*           value lists on Text1 ("Risk"), Number1, Cost1, Date1; two lists
//   sc-*           Phase summarising Design 2 / Build 5 / Test 7 in Number1 (Cost1,
//                  Flag1, Date1) under each PjSummaryCalc; Project's summary
//                  values are its "SUMMARY" log lines
//   vl-*           value list options: not restricted, default value, order
//   gf-*           indicator rules comparing with another field
//   fl-*, nb-1-fl-11  Flag1 / Flag11 / Flag20 set on Design (and Number1 = 5)
//   gx-*           graphical indicators on Number1, Cost1, Date1, Flag1, with a
//                  formula, two rules, "any value", summary rows inheriting

#include "codec/customfielddefs.h"
#include "codec/propsreader.h"
#include "model/customfieldlogic.h"
#include "model/project.h"
#include "model/scheduler.h"
#include "model/task.h"
#include "ole/compoundfile.h"
#include "mppio.h"
#include "xmlio.h"

#include <QFile>
#include <QtEndian>
#include <QRegularExpression>
#include <QtTest>

using namespace schedule;

namespace {

const QString kDir = QStringLiteral(SCHEDULEIO_FIXTURE_DIR "/mpp_samples/customfields/");
const QString kStorage = QStringLiteral("   114");

QByteArray fixture(const QString &name)
{
    QFile f(kDir + name);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = f.readAll();
    return bytes.startsWith("version https://git-lfs") ? QByteArray() : bytes;
}

Project open(const QByteArray &bytes)
{
    MppIO io;
    if (!io.openFromData(bytes)) return {};
    return io.project();
}

QByteArray propsItem(const QByteArray &file, const QStringList &path, quint32 key)
{
    CompoundFile cf;
    if (!cf.openFromData(file)) return {};
    PropsReader pr;
    pr.parse(cf.readStream(path));
    return pr.value(key);
}

const Task *taskNamed(const Project &p, const QString &name)
{
    for (const Task &t : p.tasks)
        if (t.name == name) return &t;
    return nullptr;
}

QVariant valueOf(const Task &t, const QString &field)
{
    for (const CustomField &f : t.customFields)
        if (f.name == field) return f.value;
    return QVariant();
}

const CustomField *definition(const Project &p, quint32 fieldId)
{
    for (const CustomField &d : p.customFieldDefinitions)
        if (quint32(d.fieldId) == fieldId) return &d;
    return nullptr;
}

Task sampleTask()
{
    Task t;
    t.uniqueId = 7; t.id = 3; t.name = QStringLiteral("Build"); t.cost = 1250;
    t.start = QDateTime(QDate(2026, 11, 17), QTime(8, 0));
    t.finish = QDateTime(QDate(2026, 11, 23), QTime(17, 0));
    t.durationMillis = 5LL * 480 * 60000;
    t.percentComplete = 0.4;
    CustomField risk; risk.fieldId = 0x0B400033; risk.name = QStringLiteral("Risk");
    risk.value = QStringLiteral("Low");
    t.customFields = { risk };
    return t;
}

QString eval(const QString &formula, QString *error = nullptr)
{
    Project p;
    const Task t = sampleTask();
    p.tasks = { t };
    QString e;
    const QVariant v = CustomFieldLogic::evaluate(p, t, formula, &e);
    if (error) *error = e;
    if (v.typeId() == QMetaType::Double) return QString::number(v.toDouble(), 'g', 12);
    if (v.typeId() == QMetaType::Bool) return v.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    if (v.typeId() == QMetaType::QDateTime) return v.toDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    return v.toString();
}

} // namespace

class TstCustomFieldFormulas : public QObject
{
    Q_OBJECT
private slots:
    void textComparesAsText();
    void iifOnlyEvaluatesTheBranchTaken();
    void operatorsFollowVba();
    void functions();
    void errorsAreReported();
    void translatesFieldReferences();
    void projectValuesForEveryFormula_data();
    void projectValuesForEveryFormula();
    void summaryTasksAreNotCalculated();
    void readsAliasesAndFormulas();
    void writesProjectsBytes();
    void roundTripsThroughMpp();
    void mspdiUsesFieldNameAliasAndFieldIds();
    void readsProjectsValueLists();
    void readsProjectsIndicators();
    void writesProjectsIndicatorBytes();
    void valueListsAndIndicatorsRoundTripThroughMpp();
    void summaryRowsRollUpAsProjectDoes_data();
    void summaryRowsRollUpAsProjectDoes();
    void readsValueListOptions();
    void writesProjectsLookupDirectory();
    void indicatorsCompareWithAnotherField();
    void optionsRoundTripThroughMpp();
    void mspdiSummaryCalculation();
    void flagsAndNumberPresenceSitWhereProjectPutsThem();
};

void TstCustomFieldFormulas::textComparesAsText()
{
    // Every text compared equal before (both converted to 0).
    QCOMPARE(eval(QStringLiteral("IIf([Risk]=\"High\",1,0)")), QStringLiteral("0"));
    QCOMPARE(eval(QStringLiteral("IIf([Risk]=\"Low\",1,0)")), QStringLiteral("1"));
    QCOMPARE(eval(QStringLiteral("[Name]<>\"Design\"")), QStringLiteral("True"));
    QCOMPARE(eval(QStringLiteral("\"10\" > 9")), QStringLiteral("True"));
}

void TstCustomFieldFormulas::iifOnlyEvaluatesTheBranchTaken()
{
    QString error;
    QCOMPARE(eval(QStringLiteral("IIf([Work]=0,0,[Cost]/[Work])"), &error), QStringLiteral("0"));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(eval(QStringLiteral("Switch([Cost]>2000,1,[Cost]>1000,2,True,3)")), QStringLiteral("2"));
    QCOMPARE(eval(QStringLiteral("Choose(2,\"a\",\"b\",1/0)")), QStringLiteral("b"));
}

void TstCustomFieldFormulas::operatorsFollowVba()
{
    QCOMPARE(eval(QStringLiteral("7 Mod 3 + 2^3 + 7\\2")), QStringLiteral("12"));
    QCOMPARE(eval(QStringLiteral("-2^2")), QStringLiteral("-4"));
    QCOMPARE(eval(QStringLiteral("[Cost]>100 And [Duration]>0")), QStringLiteral("True"));
    QCOMPARE(eval(QStringLiteral("Not ([Cost]>100) Or False")), QStringLiteral("False"));
    QCOMPARE(eval(QStringLiteral("True + 0")), QStringLiteral("-1"));
    QCOMPARE(eval(QStringLiteral("[Name] & \" (\" & [% Complete] & \"%)\"")), QStringLiteral("Build (40%)"));
    QCOMPARE(eval(QStringLiteral("[Percent Complete]")), QStringLiteral("40"));
    QCOMPARE(eval(QStringLiteral("[Finish]-[Start]")), QStringLiteral("6.375"));
    QCOMPARE(eval(QStringLiteral("[Start]+7")), QStringLiteral("2026-11-24 08:00"));
    QCOMPARE(eval(QStringLiteral("[Start] & \"\"")), QStringLiteral("11/17/2026 8:00:00 AM"));
    QCOMPARE(eval(QStringLiteral("[Name] Like \"B*d\"")), QStringLiteral("True"));
    QCOMPARE(eval(QStringLiteral("[Deadline] = 4294967295")), QStringLiteral("True"));
    QCOMPARE(eval(QStringLiteral("[Deadline] & \"\"")), QStringLiteral("NA"));
    QCOMPARE(eval(QStringLiteral("#12/25/2026# - [Start] > 30")), QStringLiteral("True"));
    QCOMPARE(eval(QStringLiteral("\"a\" + \"b\"")), QStringLiteral("ab"));
}

void TstCustomFieldFormulas::functions()
{
    QCOMPARE(eval(QStringLiteral("Left([Name],3) & UCase(Right([Name],2))")), QStringLiteral("BuiLD"));
    QCOMPARE(eval(QStringLiteral("Len([Name]) + InStr([Name],\"i\")")), QStringLiteral("8"));
    QCOMPARE(eval(QStringLiteral("Mid([Name],2,3) & \"|\" & Trim(\"  x  \") & \"|\" & LCase(\"AbC\")")),
             QStringLiteral("uil|x|abc"));
    QCOMPARE(eval(QStringLiteral("Abs(-2) + Int(2.7) + Fix(-2.7) + Round(2.5) + Round(1.25,1)")),
             QStringLiteral("5.2"));
    QCOMPARE(eval(QStringLiteral("DateDiff(\"d\",[Start],[Finish])")), QStringLiteral("6"));
    QCOMPARE(eval(QStringLiteral("DateDiff(\"m\",#1/31/2026#,#2/1/2026#)")), QStringLiteral("1"));
    QCOMPARE(eval(QStringLiteral("DateAdd(\"m\",1,#1/31/2026#)")), QStringLiteral("2026-02-28 00:00"));
    QCOMPARE(eval(QStringLiteral("Year([Start])*100+Month([Start])")), QStringLiteral("202611"));
    QCOMPARE(eval(QStringLiteral("Weekday([Start])")), QStringLiteral("3"));   // Tuesday
    QCOMPARE(eval(QStringLiteral("Format([Cost],\"0.0\")")), QStringLiteral("1250.0"));
    QCOMPARE(eval(QStringLiteral("Format([Cost],\"#,##0.00\")")), QStringLiteral("1,250.00"));
    QCOMPARE(eval(QStringLiteral("Format([Cost],\"Currency\")")), QStringLiteral("$1,250.00"));
    QCOMPARE(eval(QStringLiteral("Format(0.256,\"0%\")")), QStringLiteral("26%"));
    QCOMPARE(eval(QStringLiteral("Format([Start],\"mmm d, yyyy\")")), QStringLiteral("Nov 17, 2026"));
    QCOMPARE(eval(QStringLiteral("Format([Start],\"dddd hh:nn\")")), QStringLiteral("Tuesday 08:00"));
    QCOMPARE(eval(QStringLiteral("ProjDateDiff([Start],[Finish])")), QStringLiteral("2400"));
    QCOMPARE(eval(QStringLiteral("ProjDateAdd([Start],960)")), QStringLiteral("2026-11-18 17:00"));
    QCOMPARE(eval(QStringLiteral("ProjDurConv([Duration],pjDays)")), QStringLiteral("5 days"));
    QCOMPARE(eval(QStringLiteral("ProjDurValue(\"2d\")")), QStringLiteral("960"));
    QCOMPARE(eval(QStringLiteral("IsNumeric(\"12\") And Not IsNumeric(\"x\")")), QStringLiteral("True"));
    QCOMPARE(eval(QStringLiteral("CStr(1.5) & Str(2) & Val(\"42abc\")")), QStringLiteral("1.5 242"));
}

void TstCustomFieldFormulas::errorsAreReported()
{
    Project p;
    p.tasks = { sampleTask() };
    QString error;
    QVERIFY(!CustomFieldLogic::evaluate(p, p.tasks.first(), QStringLiteral("[Cost]/0"), &error).isValid());
    QCOMPARE(error, QStringLiteral("Division by zero"));
    QVERIFY(CustomFieldLogic::validate(p, QStringLiteral("[Cost]/0")).isEmpty());
    QVERIFY(CustomFieldLogic::validate(p, QStringLiteral("[Nope] + 1")).contains(QStringLiteral("[Nope]")));
    QVERIFY(CustomFieldLogic::validate(p, QStringLiteral("IIf([Cost]>1, 2")).contains(QStringLiteral(")")));
    QVERIFY(CustomFieldLogic::validate(p, QStringLiteral("Cost * 2")).contains(QStringLiteral("brackets")));
    QVERIFY(CustomFieldLogic::validate(p, QStringLiteral("Frob(1)")).contains(QStringLiteral("Frob")));
    QVERIFY(CustomFieldLogic::validate(p, QStringLiteral("IIf(1,2)")).contains(QStringLiteral("IIf")));
    QVERIFY(CustomFieldLogic::validate(p, QStringLiteral("[Risk] & [Text4] & [MSPJ188743685]")).isEmpty());
}

void TstCustomFieldFormulas::translatesFieldReferences()
{
    Project p;
    CustomField score; score.fieldId = 0x0B400057; score.name = QStringLiteral("Score");
    p.customFieldDefinitions = { score };
    const QString typed = QStringLiteral("IIf([Score]>[Cost], \"[Name]\", [Text2] & [% Complete])");
    const QString stored = CustomFieldLogic::toProjectFormula(p, typed);
    QCOMPARE(stored, QStringLiteral("IIf([MSPJ188743767]>[MSPJ188743685], \"[Name]\", "
                                    "[MSPJ188743734] & [MSPJ188743712])"));
    QCOMPARE(CustomFieldLogic::fromProjectFormula(p, stored),
             QStringLiteral("IIf([Score]>[Cost], \"[Name]\", [Text2] & [% Complete])"));
    // Project's classic Start/Finish/Duration ids read as the same fields.
    QCOMPARE(CustomFieldLogic::fromProjectFormula(p, QStringLiteral("[MSPJ188743715]-[MSPJ188744965]")),
             QStringLiteral("[Start]-[Start]"));
}

void TstCustomFieldFormulas::projectValuesForEveryFormula_data()
{
    QTest::addColumn<QString>("field");
    QTest::addColumn<QStringList>("expected");   // Design, Build, Test, Review item
    QTest::newRow("Number1") << "Number1" << QStringList{ "10", "1", "1", "1" };
    QTest::newRow("Number2") << "Number2" << QStringList{ "2", "5", "0", "3" };
    QTest::newRow("Number3") << "Number3" << QStringList{ "0", "0", "0", "0" };
    QTest::newRow("Text1") << "Text1" << QStringList{ "Design (0%)", "Build (40%)", "Test (0%)", "Review item (0%)" };
    QTest::newRow("Text2") << "Text2" << QStringList{ "no", "yes", "no", "no" };
    QTest::newRow("Flag1") << "Flag1" << QStringList{ "true", "true", "false", "false" };
    QTest::newRow("Number4") << "Number4" << QStringList{ "1", "6", "0", "2" };
    QTest::newRow("Text3") << "Text3" << QStringList{ "DesGN", "BuiLD", "TesST", "RevEM" };
    QTest::newRow("Number5") << "Number5" << QStringList{ "3", "2", "1", "1" };
    QTest::newRow("Date1") << "Date1" << QStringList{ "2026-11-24", "2026-11-24", "2026-11-24", "2026-11-24" };
    QTest::newRow("Number6") << "Number6" << QStringList{ "1.375", "6.375", "0", "2.375" };
    QTest::newRow("Number7") << "Number7" << QStringList{ "12", "12", "12", "12" };
    QTest::newRow("Text4") << "Text4" << QStringList{ "1500", "200", "0", "0" };
    QTest::newRow("Text5") << "Text5" << QStringList{ "960", "2400", "0", "1440" };
    QTest::newRow("Text6") << "Text6" << QStringList{ "11/17/2026 8:00:00 AM", "11/17/2026 8:00:00 AM",
                                                      "11/17/2026 8:00:00 AM", "11/17/2026 8:00:00 AM" };
    QTest::newRow("Number8") << "Number8" << QStringList{ "10", "8", "4", "15" };
    QTest::newRow("Text7") << "Text7" << QStringList{ "False/False/False", "False/False/True",
                                                      "False/True/False", "False/False/False" };
    QTest::newRow("Text8") << "Text8" << QStringList{ "1500.0", "200.0", "0.0", "0.0" };
    QTest::newRow("Number9") << "Number9" << QStringList{ "2000", "700", "500", "500" };
    QTest::newRow("Text9") << "Text9" << QStringList{ "esi|x|abc", "uil|x|abc", "est|x|abc", "evi|x|abc" };
    QTest::newRow("Number10") << "Number10" << QStringList{ "2", "2", "3", "2" };
    QTest::newRow("Text10") << "Text10" << QStringList{ "leaf", "leaf", "leaf", "leaf" };
    QTest::newRow("Number11") << "Number11" << QStringList{ "960", "2400", "0", "1440" };
    QTest::newRow("Text11") << "Text11" << QStringList{ "1:2:2:1.1", "2:3:2:1.2", "3:4:2:1.3", "5:5:2:1.4" };
    QTest::newRow("Number12") << "Number12" << QStringList{ "2880", "0", "4800", "1920" };
}

void TstCustomFieldFormulas::projectValuesForEveryFormula()
{
    QFETCH(QString, field);
    QFETCH(QStringList, expected);
    const QByteArray bytes = fixture(QStringLiteral("cf-eval.mpp"));
    if (bytes.isEmpty()) QSKIP("cf-eval.mpp fixture unavailable");
    Project p = open(bytes);
    QCOMPARE(p.customFieldDefinitions.size(), 25);
    // As in the app: slack and Critical come from the schedule, then formulas.
    Scheduler::computeSlack(p);
    CustomFieldLogic::recalculate(p);
    const QStringList names = { "Design", "Build", "Test", "Review item" };
    for (int i = 0; i < names.size(); ++i) {
        const Task *t = taskNamed(p, names.at(i));
        QVERIFY(t);
        const QVariant v = valueOf(*t, field);
        QString got;
        if (v.typeId() == QMetaType::Double) got = QString::number(v.toDouble(), 'g', 12);
        else if (v.typeId() == QMetaType::QDateTime) got = v.toDateTime().date().toString(Qt::ISODate);
        else got = v.toString();
        QVERIFY2(got == expected.at(i),
                 qPrintable(QStringLiteral("%1 on %2: got \"%3\", Project shows \"%4\"")
                                .arg(field, names.at(i), got, expected.at(i))));
    }
}

void TstCustomFieldFormulas::summaryTasksAreNotCalculated()
{
    const QByteArray bytes = fixture(QStringLiteral("cf-eval.mpp"));
    if (bytes.isEmpty()) QSKIP("cf-eval.mpp fixture unavailable");
    Project p = open(bytes);
    CustomFieldLogic::recalculate(p);
    const Task *phase = taskNamed(p, QStringLiteral("Phase"));
    QVERIFY(phase && phase->summary);
    QVERIFY(!valueOf(*phase, QStringLiteral("Number1")).isValid());
    QVERIFY(!valueOf(*phase, QStringLiteral("Text1")).isValid());
}

void TstCustomFieldFormulas::readsAliasesAndFormulas()
{
    const QByteArray alias = fixture(QStringLiteral("cf-alias.mpp"));
    const QByteArray ref = fixture(QStringLiteral("cf-formula-ref.mpp"));
    if (alias.isEmpty() || ref.isEmpty()) QSKIP("fixtures unavailable");

    Project a = open(alias);
    const CustomField *risk = definition(a, 0x0B400033);
    QVERIFY(risk);
    QCOMPARE(risk->name, QStringLiteral("Risk"));
    QVERIFY(risk->formula.isEmpty());
    QCOMPARE(valueOf(*taskNamed(a, QStringLiteral("Design")), QStringLiteral("Risk")).toString(),
             QStringLiteral("High"));

    Project r = open(ref);
    QCOMPARE(definition(r, 0x0B400057)->name, QStringLiteral("Score"));
    QCOMPARE(definition(r, 0x0B400057)->formula, QStringLiteral("[Cost]*2"));
    QCOMPARE(definition(r, 0x0B400058)->name, QStringLiteral("Double"));
    QCOMPARE(definition(r, 0x0B400058)->formula, QStringLiteral("[Score]*2"));
    CustomFieldLogic::recalculate(r);
    QCOMPARE(valueOf(*taskNamed(r, QStringLiteral("Design")), QStringLiteral("Double")).toDouble(), 6000.0);
    QCOMPARE(valueOf(*taskNamed(r, QStringLiteral("Build")), QStringLiteral("Double")).toDouble(), 800.0);
}

void TstCustomFieldFormulas::writesProjectsBytes()
{
    // The blocks we write for Project's own definitions are its bytes exactly.
    for (const QString &name : { QStringLiteral("cf-formula-ref.mpp"), QStringLiteral("cf-kinds.mpp"),
                                 QStringLiteral("cf-formula-text.mpp"), QStringLiteral("cf-eval.mpp") }) {
        const QByteArray file = fixture(name);
        if (file.isEmpty()) QSKIP("fixtures unavailable");
        const QByteArray formulas = propsItem(file, { kStorage, QStringLiteral("Props") },
                                              CustomFieldDefs::kFormulasKey);
        const QByteArray defs = propsItem(file, { kStorage, QStringLiteral("TBkndTask"), QStringLiteral("Props") },
                                          CustomFieldDefs::kTaskDefinitionsKey);
        const QList<CustomFieldDefs::Formula> read = CustomFieldDefs::readFormulas(formulas);
        const QByteArray rewritten = CustomFieldDefs::writeFormulas(read);
        // Project leaves stale bytes in a record's padding; compare the rest.
        QCOMPARE(rewritten.size(), formulas.size());
        QCOMPARE(CustomFieldDefs::readFormulas(rewritten).size(), read.size());
        for (int i = 0; i < read.size(); ++i) {
            QCOMPARE(CustomFieldDefs::readFormulas(rewritten).at(i).text, read.at(i).text);
            QCOMPARE(CustomFieldDefs::readFormulas(rewritten).at(i).fieldId, read.at(i).fieldId);
        }
        QCOMPARE(rewritten.left(8 + 8 * read.size()), formulas.left(8 + 8 * read.size()));
        if (name != QLatin1String("cf-kinds.mpp")) {   // cf-kinds has a failed formula's record
            QCOMPARE(CustomFieldDefs::writeTaskDefinitions(defs, CustomFieldDefs::readAliases(defs),
                                                           CustomFieldDefs::readAttributeFields(defs))
                         .toHex(' '),
                     defs.toHex(' '));
        }
    }
}

void TstCustomFieldFormulas::roundTripsThroughMpp()
{
    const QByteArray base = fixture(QStringLiteral("cf-base.mpp"));
    if (base.isEmpty()) QSKIP("cf-base.mpp fixture unavailable");
    Project p = open(base);
    CustomField risk; risk.fieldId = 0x0B400033; risk.name = QStringLiteral("Risk");
    CustomField score; score.fieldId = 0x0B400057; score.name = QStringLiteral("Score");
    score.formula = QStringLiteral("IIf([Risk]=\"High\", [Cost]*2, [Cost])");
    CustomField late; late.fieldId = 0x0B400048; late.formula = QStringLiteral("[Finish] > #11/20/2026#");
    p.customFieldDefinitions = { risk, score, late };
    for (Task &t : p.tasks) {
        CustomField v = risk;
        v.value = t.name == QLatin1String("Design") ? QStringLiteral("High") : QStringLiteral("Low");
        t.customFields.append(v);
    }
    CustomFieldLogic::recalculate(p);

    MppIO io;
    io.setProject(p);
    const QByteArray saved = io.saveToData();
    QVERIFY(!saved.isEmpty());
    // Project's spelling on disk.
    const QList<CustomFieldDefs::Formula> stored = CustomFieldDefs::readFormulas(
        propsItem(saved, { kStorage, QStringLiteral("Props") }, CustomFieldDefs::kFormulasKey));
    QCOMPARE(stored.size(), 2);
    QCOMPARE(stored.at(0).text, QStringLiteral("[MSPJ188744966] > #11/20/2026#"));
    QCOMPARE(stored.at(1).text, QStringLiteral("IIf([MSPJ188743731]=\"High\", [MSPJ188743685]*2, [MSPJ188743685])"));
    QVERIFY(stored.at(0).active && stored.at(1).active);

    Project back = open(saved);
    QCOMPARE(definition(back, 0x0B400033)->name, QStringLiteral("Risk"));
    QCOMPARE(definition(back, 0x0B400057)->name, QStringLiteral("Score"));
    QCOMPARE(definition(back, 0x0B400057)->formula, score.formula);
    QCOMPARE(definition(back, 0x0B400048)->name, QStringLiteral("Flag1"));
    QCOMPARE(definition(back, 0x0B400048)->formula, late.formula);
    CustomFieldLogic::recalculate(back);
    QCOMPARE(valueOf(*taskNamed(back, QStringLiteral("Design")), QStringLiteral("Score")).toDouble(), 3000.0);
    QCOMPARE(valueOf(*taskNamed(back, QStringLiteral("Build")), QStringLiteral("Score")).toDouble(), 200.0);
    QCOMPARE(valueOf(*taskNamed(back, QStringLiteral("Design")), QStringLiteral("Risk")).toString(),
             QStringLiteral("High"));
}

void TstCustomFieldFormulas::mspdiUsesFieldNameAliasAndFieldIds()
{
    QFile f(kDir + QStringLiteral("cf-formula-ref.xml"));
    if (!f.open(QIODevice::ReadOnly)) QSKIP("cf-formula-ref.xml fixture unavailable");
    XmlIO io;
    QVERIFY(io.openFromData(f.readAll()));
    const Project p = io.project();
    QVERIFY(definition(p, 0x0B400058));
    QCOMPARE(definition(p, 0x0B400058)->name, QStringLiteral("Double"));
    QCOMPARE(definition(p, 0x0B400058)->formula, QStringLiteral("[Score]*2"));
    const QString xml = QString::fromUtf8(io.saveToData());
    QVERIFY(xml.contains(QStringLiteral("<FieldName>Number2</FieldName>")));
    QVERIFY(xml.contains(QStringLiteral("<Alias>Double</Alias>")));
    QVERIFY(xml.contains(QStringLiteral("<Formula>[MSPJ188743767]*2</Formula>")));
}

void TstCustomFieldFormulas::readsProjectsValueLists()
{
    struct Case { const char *file; quint32 field; QStringList values; };
    const Case cases[] = {
        { "lk-text2.mpp", 0x0B400033, { "Low", "High" } },
        { "lk-number.mpp", 0x0B400057, { "1", "2.5", "3" } },
        { "lk-cost.mpp", 0x0B40006A, { "100", "250" } },
        { "lk-date.mpp", 0x0B400109, { "2027-01-15 08:00", "2027-02-01 08:00" } },
    };
    for (const Case &c : cases) {
        const QByteArray bytes = fixture(QLatin1String(c.file));
        if (bytes.isEmpty()) QSKIP("fixtures unavailable");
        const Project p = open(bytes);
        const CustomField *d = definition(p, c.field);
        QVERIFY2(d, c.file);
        QCOMPARE(d->lookupValues, c.values);
        QVERIFY(d->formula.isEmpty());
    }
    // Two fields, two lists.
    const Project two = open(fixture(QStringLiteral("lk-two.mpp")));
    QCOMPARE(definition(two, 0x0B400033)->lookupValues, QStringList({ "Low", "High" }));
    QCOMPARE(definition(two, 0x0B400036)->lookupValues, QStringList({ "Yes", "No" }));
    // The value list's descriptions are Project's; the alias is kept too.
    QCOMPARE(definition(open(fixture(QStringLiteral("lk-text2.mpp"))), 0x0B400033)->name,
             QStringLiteral("Risk"));
}

void TstCustomFieldFormulas::readsProjectsIndicators()
{
    struct Case { const char *file; quint32 field; QString comparison; QString value; QString image; };
    const Case cases[] = {
        { "gx-inherit.mpp", 0x0B400057, "gt", "10", "sphere-lime" },
        { "gx-date.mpp", 0x0B400109, "gt", "2027-01-15 17:00", "sphere-red" },
        { "gx-flag.mpp", 0x0B400048, "eq", "Yes", "sphere-red" },
        { "gx-cost.mpp", 0x0B40006A, "gt", "100", "sphere-red" },
        { "gx-any.mpp", 0x0B400057, "any", "", "sphere-lime" },
    };
    for (const Case &c : cases) {
        const QByteArray bytes = fixture(QLatin1String(c.file));
        if (bytes.isEmpty()) QSKIP("fixtures unavailable");
        const Project p = open(bytes);
        const CustomField *d = definition(p, c.field);
        QVERIFY2(d && d->graphicalIndicators.size() == 1, c.file);
        QCOMPARE(d->graphicalIndicators.first().comparison, c.comparison);
        QCOMPARE(d->graphicalIndicators.first().value.toString(), c.value);
        QCOMPARE(d->graphicalIndicators.first().indicator, c.image);
    }
    const Project two = open(fixture(QStringLiteral("gx-two.mpp")));
    const CustomField *d = definition(two, 0x0B400057);
    QCOMPARE(d->graphicalIndicators.size(), 2);
    QCOMPARE(d->graphicalIndicators.at(1).comparison, QStringLiteral("lt"));
    QCOMPARE(d->graphicalIndicators.at(1).value.toString(), QStringLiteral("5"));
    QCOMPARE(d->graphicalIndicators.at(0).indicator, QStringLiteral("sphere-red"));
    // A formula and its indicators together.
    const Project f = open(fixture(QStringLiteral("gx-formula.mpp")));
    QCOMPARE(definition(f, 0x0B400057)->formula, QStringLiteral("[Cost]"));
    QCOMPARE(definition(f, 0x0B400057)->graphicalIndicators.size(), 1);
}

void TstCustomFieldFormulas::writesProjectsIndicatorBytes()
{
    // gx-inherit is Project's default (summary rows inherit the rules), which
    // is what we write: the record is Project's byte for byte.
    const QByteArray file = fixture(QStringLiteral("gx-inherit.mpp"));
    if (file.isEmpty()) QSKIP("fixture unavailable");
    const QByteArray formulas = propsItem(file, { kStorage, QStringLiteral("Props") },
                                          CustomFieldDefs::kFormulasKey);
    QCOMPARE(CustomFieldDefs::writeFormulas(CustomFieldDefs::readFormulas(formulas)).toHex(' '),
             formulas.toHex(' '));
    const QByteArray defs = propsItem(file, { kStorage, QStringLiteral("TBkndTask"), QStringLiteral("Props") },
                                      CustomFieldDefs::kTaskDefinitionsKey);
    QCOMPARE(CustomFieldDefs::writeTaskDefinitions(defs, CustomFieldDefs::readAliases(defs),
                                                   CustomFieldDefs::readAttributeFields(defs)).toHex(' '),
             defs.toHex(' '));
    // A value list's record and attribute record match Project's except the
    // list GUID, which is new each time.
    const QByteArray lk = fixture(QStringLiteral("lk-text2.mpp"));
    const QByteArray lkFormulas = propsItem(lk, { kStorage, QStringLiteral("Props") },
                                            CustomFieldDefs::kFormulasKey);
    QCOMPARE(CustomFieldDefs::writeFormulas(CustomFieldDefs::readFormulas(lkFormulas)).toHex(' '),
             lkFormulas.toHex(' '));
}

void TstCustomFieldFormulas::valueListsAndIndicatorsRoundTripThroughMpp()
{
    const QByteArray base = fixture(QStringLiteral("cf-base.mpp"));
    if (base.isEmpty()) QSKIP("cf-base.mpp fixture unavailable");
    Project p = open(base);
    CustomField risk; risk.fieldId = 0x0B400033; risk.name = QStringLiteral("Risk");
    risk.lookupValues = { "Low", "Medium", "High" };
    risk.graphicalIndicators = { { "eq", QStringLiteral("High"), "critical" },
                                 { "contains", QStringLiteral("Med"), "sphere-yellow" } };
    CustomField size; size.fieldId = 0x0B400057; size.name = QStringLiteral("Size");
    size.lookupValues = { "1", "2.5" };
    CustomField score; score.fieldId = 0x0B400058; score.name = QStringLiteral("Score");
    score.formula = QStringLiteral("[Cost] / 100");
    score.graphicalIndicators = { { "ge", 10, "flag-red" }, { "lt", 10, "check-mark" } };
    CustomField budget; budget.fieldId = 0x0B40006A; budget.lookupValues = { "100", "250.5" };
    CustomField due; due.fieldId = 0x0B400109;
    due.graphicalIndicators = { { "lt", QStringLiteral("2026-12-01"), "sphere-red" } };
    p.customFieldDefinitions = { risk, size, score, budget, due };
    MppIO io;
    io.setProject(p);
    const Project back = open(io.saveToData());

    QCOMPARE(definition(back, 0x0B400033)->name, QStringLiteral("Risk"));
    QCOMPARE(definition(back, 0x0B400033)->lookupValues, risk.lookupValues);
    QCOMPARE(definition(back, 0x0B400033)->graphicalIndicators.size(), 2);
    QCOMPARE(definition(back, 0x0B400033)->graphicalIndicators.at(0).indicator, QStringLiteral("sphere-red"));
    QCOMPARE(definition(back, 0x0B400033)->graphicalIndicators.at(1).comparison, QStringLiteral("contains"));
    QCOMPARE(definition(back, 0x0B400033)->graphicalIndicators.at(1).value.toString(), QStringLiteral("Med"));
    QCOMPARE(definition(back, 0x0B400057)->lookupValues, size.lookupValues);
    QCOMPARE(definition(back, 0x0B400058)->formula, score.formula);
    QCOMPARE(definition(back, 0x0B400058)->graphicalIndicators.at(0).comparison, QStringLiteral("ge"));
    QCOMPARE(definition(back, 0x0B400058)->graphicalIndicators.at(0).value.toString(), QStringLiteral("10"));
    QCOMPARE(definition(back, 0x0B400058)->graphicalIndicators.at(1).indicator, QStringLiteral("check-mark"));
    QCOMPARE(definition(back, 0x0B40006A)->lookupValues, budget.lookupValues);
    QCOMPARE(definition(back, 0x0B400109)->graphicalIndicators.at(0).value.toString(), QStringLiteral("2026-12-01"));
}

void TstCustomFieldFormulas::summaryRowsRollUpAsProjectDoes_data()
{
    QTest::addColumn<QString>("file");
    QTest::addColumn<quint32>("field");
    QTest::addColumn<int>("calc");
    QTest::addColumn<QString>("phase");   // what Project shows on the summary
    QTest::newRow("maximum") << "sc-0.mpp" << 0x0B400057u << 0 << "7";
    QTest::newRow("minimum") << "sc-1.mpp" << 0x0B400057u << 1 << "2";
    QTest::newRow("count all") << "sc-2.mpp" << 0x0B400057u << 2 << "3";
    QTest::newRow("sum") << "sc-3.mpp" << 0x0B400057u << 3 << "14";
    QTest::newRow("average") << "sc-4.mpp" << 0x0B400057u << 4 << "4.67";
    QTest::newRow("average first sublevel") << "sc-5.mpp" << 0x0B400057u << 5 << "4.67";
    QTest::newRow("count first sublevel") << "sc-6.mpp" << 0x0B400057u << 6 << "3";
    QTest::newRow("count non-summaries") << "sc-7.mpp" << 0x0B400057u << 7 << "3";
    QTest::newRow("use formula") << "sc-11.mpp" << 0x0B400057u << 11 << "5";
    QTest::newRow("cost sum") << "sc-cost-3.mpp" << 0x0B40006Au << 3 << "14";
    QTest::newRow("flag or") << "sc-flag-or.mpp" << 0x0B400048u << 0 << "true";
    QTest::newRow("flag and") << "sc-flag-and.mpp" << 0x0B400048u << 1 << "false";
    QTest::newRow("date latest") << "sc-date-max.mpp" << 0x0B400109u << 0 << "Test";
}

void TstCustomFieldFormulas::summaryRowsRollUpAsProjectDoes()
{
    QFETCH(QString, file);
    QFETCH(quint32, field);
    QFETCH(int, calc);
    QFETCH(QString, phase);
    const QByteArray bytes = fixture(file);
    if (bytes.isEmpty()) QSKIP("fixture unavailable");
    Project p = open(bytes);
    const CustomField *d = definition(p, field);
    QVERIFY(d);
    QCOMPARE(d->summaryCalc, calc);
    // Recalculate from scratch: the summary's stored value is cleared first.
    for (Task &t : p.tasks)
        if (t.summary)
            for (CustomField &f : t.customFields) f.value = QVariant();
    Scheduler::computeSlack(p);
    CustomFieldLogic::recalculate(p);
    const Task *summary = taskNamed(p, QStringLiteral("Phase"));
    QVERIFY(summary);
    QVariant v;
    for (const CustomField &f : summary->customFields)
        if (quint32(f.fieldId) == field) v = f.value;
    if (phase == QLatin1String("Test")) {   // the latest date is the Test task's
        QVariant test;
        for (const CustomField &f : taskNamed(p, QStringLiteral("Test"))->customFields)
            if (quint32(f.fieldId) == field) test = f.value;
        QCOMPARE(v.toDateTime(), test.toDateTime());
    } else if (v.typeId() == QMetaType::Bool) {
        QCOMPARE(v.toBool() ? QStringLiteral("true") : QStringLiteral("false"), phase);
    } else {
        QCOMPARE(QString::number(v.toDouble(), 'f', 2).remove(QRegularExpression(QStringLiteral("\\.?0+$"))),
                 phase);
    }
}

void TstCustomFieldFormulas::readsValueListOptions()
{
    const Project plain = open(fixture(QStringLiteral("lk-text2.mpp")));
    if (plain.tasks.isEmpty()) QSKIP("fixtures unavailable");
    const CustomField *d = definition(plain, 0x0B400033);
    QCOMPARE(d->lookupDescriptions, QStringList({ "Low risk", "High risk" }));
    QVERIFY(d->restrictToList);
    QCOMPARE(d->listOrder, 0);
    QVERIFY(d->defaultValue.isEmpty());

    QVERIFY(!definition(open(fixture(QStringLiteral("vl-free.mpp"))), 0x0B400033)->restrictToList);
    QCOMPARE(definition(open(fixture(QStringLiteral("vl-default.mpp"))), 0x0B400033)->defaultValue,
             QStringLiteral("High"));
    QCOMPARE(definition(open(fixture(QStringLiteral("vl-asc.mpp"))), 0x0B400033)->listOrder, 1);
    QCOMPARE(definition(open(fixture(QStringLiteral("vl-desc.mpp"))), 0x0B400033)->listOrder, 2);
    QCOMPARE(definition(open(fixture(QStringLiteral("vl-free.mpp"))), 0x0B400033)->lookupDescriptions,
             QStringList({ "Low risk" }));
}

void TstCustomFieldFormulas::writesProjectsLookupDirectory()
{
    for (const QString &name : { QStringLiteral("vl-free.mpp"), QStringLiteral("vl-asc.mpp"),
                                 QStringLiteral("vl-desc.mpp"), QStringLiteral("lk-two.mpp") }) {
        const QByteArray file = fixture(name);
        if (file.isEmpty()) QSKIP("fixtures unavailable");
        CompoundFile cf;
        QVERIFY(cf.openFromData(file));
        auto read = [&](const char *n) { return cf.readStream({ kStorage, QStringLiteral("TBkndOutlCode"), QLatin1String(n) }); };
        QHash<QByteArray, CustomFieldDefs::LookupTable> tables = CustomFieldDefs::readLookupTables(
            { read("FixedMeta"), read("FixedData"), read("Fixed2Meta"), read("Fixed2Data"),
              read("VarMeta"), read("Var2Data") });
        const QByteArray directory = propsItem(file, { kStorage, QStringLiteral("Props") },
                                               CustomFieldDefs::kLookupDirectoryKey);
        CustomFieldDefs::readLookupDirectory(directory, &tables);
        // In the directory's own order.
        QList<CustomFieldDefs::LookupTable> ordered;
        for (int o = 24; o + 38 <= directory.size(); o += 38) ordered.append(tables.value(directory.mid(o, 16)));
        QCOMPARE(CustomFieldDefs::writeLookupDirectory(directory, ordered).toHex(' '), directory.toHex(' '));
    }
}

void TstCustomFieldFormulas::indicatorsCompareWithAnotherField()
{
    struct Case { const char *file; quint32 field; QString value; };
    const Case cases[] = {
        { "gf-number.mpp", 0x0B400057, "[Number2]" },
        { "gf-date.mpp", 0x0B400109, "[Finish]" },
        { "gf-text.mpp", 0x0B400033, "[Name]" },
    };
    for (const Case &c : cases) {
        const QByteArray file = fixture(QLatin1String(c.file));
        if (file.isEmpty()) QSKIP("fixtures unavailable");
        const Project p = open(file);
        const CustomField *d = definition(p, c.field);
        QVERIFY2(d && d->graphicalIndicators.size() == 1, c.file);
        QCOMPARE(d->graphicalIndicators.first().value.toString(), c.value);
        // Project's bytes back (gf-date and gf-text also carry an empty record the
        // oracle left on Number1, which is not written back).
        if (QLatin1String(c.file) != QLatin1String("gf-number.mpp")) continue;
        const QByteArray formulas = propsItem(file, { kStorage, QStringLiteral("Props") },
                                              CustomFieldDefs::kFormulasKey);
        QCOMPARE(CustomFieldDefs::writeFormulas(CustomFieldDefs::readFormulas(formulas)).toHex(' '),
                 formulas.toHex(' '));
    }
    // Evaluated against the other field on the task.
    Project p;
    Task t = sampleTask();
    CustomField score; score.fieldId = 0x0B400057; score.name = QStringLiteral("Score"); score.value = 10.0;
    CustomField target; target.fieldId = 0x0B400058; target.name = QStringLiteral("Target"); target.value = 8.0;
    score.graphicalIndicators = { { "gt", QStringLiteral("[Target]"), "sphere-red" },
                                  { "any", QString(), "sphere-lime" } };
    t.customFields = { score, target };
    p.customFieldDefinitions = { score, target };
    p.tasks = { t };
    QCOMPARE(CustomFieldLogic::indicatorFor(p, p.tasks.first(), score), QStringLiteral("sphere-red"));
    p.tasks.first().customFields[1].value = 12.0;
    QCOMPARE(CustomFieldLogic::indicatorFor(p, p.tasks.first(), score), QStringLiteral("sphere-lime"));
}

void TstCustomFieldFormulas::optionsRoundTripThroughMpp()
{
    const QByteArray base = fixture(QStringLiteral("cf-base.mpp"));
    if (base.isEmpty()) QSKIP("cf-base.mpp fixture unavailable");
    Project p = open(base);
    CustomField risk; risk.fieldId = 0x0B400033; risk.name = QStringLiteral("Risk");
    risk.lookupValues = { "Low", "Medium", "High" };
    risk.lookupDescriptions = { "Low risk", "", "High risk" };
    risk.restrictToList = false;
    risk.listOrder = 2;
    risk.defaultValue = QStringLiteral("Medium");
    risk.graphicalIndicators = { { "eq", QStringLiteral("[Text2]"), "sphere-red" } };
    CustomField size; size.fieldId = 0x0B400057; size.summaryCalc = 3;
    size.graphicalIndicators = { { "gt", QStringLiteral("[Number2]"), "flag-red" } };
    CustomField days; days.fieldId = 0x0B400058; days.formula = QStringLiteral("[Duration]/480");
    days.summaryCalc = 11;
    CustomField late; late.fieldId = 0x0B400048; late.summaryCalc = 0;
    CustomField due; due.fieldId = 0x0B400109; due.summaryCalc = 1;
    p.customFieldDefinitions = { risk, size, days, late, due };
    MppIO io;
    io.setProject(p);
    const Project back = open(io.saveToData());
    const CustomField *r = definition(back, 0x0B400033);
    QCOMPARE(r->lookupValues, risk.lookupValues);
    QCOMPARE(r->lookupDescriptions, risk.lookupDescriptions);
    QCOMPARE(r->restrictToList, false);
    QCOMPARE(r->listOrder, 2);
    QCOMPARE(r->defaultValue, QStringLiteral("Medium"));
    QCOMPARE(r->graphicalIndicators.first().value.toString(), QStringLiteral("[Text2]"));
    QCOMPARE(definition(back, 0x0B400057)->summaryCalc, 3);
    QCOMPARE(definition(back, 0x0B400057)->graphicalIndicators.first().value.toString(),
             QStringLiteral("[Number2]"));
    QCOMPARE(definition(back, 0x0B400058)->summaryCalc, 11);
    QCOMPARE(definition(back, 0x0B400058)->formula, days.formula);
    QCOMPARE(definition(back, 0x0B400048)->summaryCalc, 0);
    QCOMPARE(definition(back, 0x0B400109)->summaryCalc, 1);
}

void TstCustomFieldFormulas::mspdiSummaryCalculation()
{
    for (const auto &c : { qMakePair(QStringLiteral("sc-3.xml"), 3), qMakePair(QStringLiteral("sc-11.xml"), 11) }) {
        QFile f(kDir + c.first);
        if (!f.open(QIODevice::ReadOnly)) QSKIP("fixtures unavailable");
        XmlIO io;
        QVERIFY(io.openFromData(f.readAll()));
        QCOMPARE(definition(io.project(), 0x0B400057)->summaryCalc, c.second);
        const QString xml = QString::fromUtf8(io.saveToData());
        if (c.second == 3) {
            QVERIFY(xml.contains(QStringLiteral("<RollupType>3</RollupType>")));
            QVERIFY(xml.contains(QStringLiteral("<CalculationType>1</CalculationType>")));
        } else {
            QVERIFY(xml.contains(QStringLiteral("<CalculationType>2</CalculationType>")));
        }
        XmlIO back;
        QVERIFY(back.openFromData(xml.toUtf8()));
        QCOMPARE(definition(back.project(), 0x0B400057)->summaryCalc, c.second);
    }
}

void TstCustomFieldFormulas::flagsAndNumberPresenceSitWhereProjectPutsThem()
{
    // Read: each flag on Design only; Number1's presence bit is not Flag11.
    struct Case { const char *file; quint32 flag; };
    for (const Case &c : { Case{ "fl-1.mpp", 0x0B400048 }, Case{ "fl-11.mpp", 0x0B400124 },
                           Case{ "fl-20.mpp", 0x0B40012D }, Case{ "nb-1-fl-11.mpp", 0x0B400124 } }) {
        const QByteArray bytes = fixture(QLatin1String(c.file));
        if (bytes.isEmpty()) QSKIP("fixtures unavailable");
        const Project p = open(bytes);
        for (const Task &t : p.tasks) {
            bool set = false, other = false;
            for (const CustomField &f : t.customFields) {
                if (quint32(f.fieldId) == c.flag) set = f.value.toBool();
                else if (f.value.typeId() == QMetaType::Bool && f.value.toBool()) other = true;
            }
            QVERIFY2(set == (t.name == QLatin1String("Design")), qPrintable(QLatin1String(c.file) + t.name));
            QVERIFY2(!other, qPrintable(QLatin1String(c.file) + t.name + " has another flag"));
        }
    }
    // Write: the bits Project itself sets (nb-1-fl-11: Number1 presence item
    // byte 25 bit 3, Flag11 item byte 33 bit 1).
    Project p = open(fixture(QStringLiteral("cf-base.mpp")));
    for (Task &t : p.tasks) {
        CustomField n; n.fieldId = 0x0B400057; n.name = QStringLiteral("Number1"); n.value = 5.0;
        t.customFields.append(n);
        if (t.name == QLatin1String("Design")) {
            CustomField f; f.fieldId = 0x0B400124; f.name = QStringLiteral("Flag11"); f.value = true;
            t.customFields.append(f);
        }
    }
    MppIO io;
    io.setProject(p);
    const QByteArray saved = io.saveToData();
    CompoundFile cf;
    QVERIFY(cf.openFromData(saved));
    const QByteArray meta = cf.readStream({ kStorage, QStringLiteral("TBkndTask"), QStringLiteral("FixedMeta") });
    const QByteArray data = cf.readStream({ kStorage, QStringLiteral("TBkndTask"), QStringLiteral("FixedData") });
    const int rows = int(qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(meta.constData() + 8)));
    int checked = 0;
    for (int r = 3; r < rows; ++r) {
        const QByteArray item = meta.mid(16 + 47 * r, 47);
        QVERIFY2(quint8(item.at(25)) & 0x08, "Number1 presence bit");
        ++checked;
    }
    QVERIFY(checked >= 3);
    const Project back = open(saved);
    for (const Task &t : back.tasks) {
        bool flag = false;
        double number = 0;
        for (const CustomField &f : t.customFields) {
            if (quint32(f.fieldId) == 0x0B400124) flag = f.value.toBool();
            if (quint32(f.fieldId) == 0x0B400057) number = f.value.toDouble();
        }
        QCOMPARE(flag, t.name == QLatin1String("Design"));
        QCOMPARE(number, 5.0);
    }
    Q_UNUSED(data);
}

QTEST_GUILESS_MAIN(TstCustomFieldFormulas)
#include "tst_customfieldformulas.moc"
