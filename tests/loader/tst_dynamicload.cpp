// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include <QLibrary>
#include <QTest>

#ifndef MPPIO_LIB_PATH
#define MPPIO_LIB_PATH ""
#endif

// Proves the headline requirement: the library can be loaded at runtime on this
// OS (.dll/.dylib/.so) and its exported C factory resolves and works. This is
// the contract a host like ProjectNotes relies on via QLibrary/dlopen.
class TstDynamicLoad : public QObject
{
    Q_OBJECT
private slots:
    void loadsAndResolvesFactory();
};

void TstDynamicLoad::loadsAndResolvesFactory()
{
    using CreateFn = MppIO *(*)();
    using DestroyFn = void (*)(MppIO *);
    using VersionFn = const char *(*)();

    QLibrary lib(QStringLiteral(MPPIO_LIB_PATH));
    QVERIFY2(lib.load(), qPrintable(lib.errorString()));

    auto create  = reinterpret_cast<CreateFn>(lib.resolve("mppio_create"));
    auto destroy = reinterpret_cast<DestroyFn>(lib.resolve("mppio_destroy"));
    auto version = reinterpret_cast<VersionFn>(lib.resolve("mppio_version"));

    QVERIFY(create);
    QVERIFY(destroy);
    QVERIFY(version);

    QCOMPARE(QString::fromLatin1(version()), QStringLiteral("0.1.0"));

    MppIO *io = create();
    QVERIFY(io != nullptr);
    // Bad input must fail cleanly through the runtime-loaded instance.
    QVERIFY(!io->openFromData(QByteArray("garbage")));
    QVERIFY(!io->errorString().isEmpty());
    destroy(io);

    QVERIFY(lib.unload());
}

QTEST_MAIN(TstDynamicLoad)
#include "tst_dynamicload.moc"
