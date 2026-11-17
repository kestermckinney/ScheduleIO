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

QTEST_GUILESS_MAIN(TstCustomFieldFormulas)
#include "tst_customfieldformulas.moc"
