// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "codec/streamquartet.h"

#include <QTest>

class TstStreamQuartet : public QObject
{
    Q_OBJECT
private slots:
    void roundTripFixedAndVar();
    void emptyQuartet();
    void truncatedVarMetaFails();
};

void TstStreamQuartet::roundTripFixedAndVar()
{
    StreamQuartet q;
    q.recordSize = 4;
    q.fixedRecords = { QByteArray("\x01\x00\x00\x00", 4),
                       QByteArray("\x02\x00\x00\x00", 4) };
    q.varEntries = {
        { 0, 1, QByteArray("hello") },
        { 1, 1, QByteArray("world!!") },
        { 1, 7, QByteArray("\xde\xad\xbe\xef", 4) },
    };

    const StreamQuartet::Streams s = q.encode();

    QString err;
    const StreamQuartet back = StreamQuartet::decode(s, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QVERIFY(back == q);
}

void TstStreamQuartet::emptyQuartet()
{
    StreamQuartet q;
    const StreamQuartet::Streams s = q.encode();
    QString err;
    const StreamQuartet back = StreamQuartet::decode(s, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QVERIFY(back == q);
}

void TstStreamQuartet::truncatedVarMetaFails()
{
    StreamQuartet q;
    q.recordSize = 0;
    q.varEntries = { { 0, 1, QByteArray("data") } };
    StreamQuartet::Streams s = q.encode();
    s.varMeta.chop(3);   // corrupt the directory
    QString err;
    StreamQuartet::decode(s, &err);
    QVERIFY(!err.isEmpty());
}

QTEST_APPLESS_MAIN(TstStreamQuartet)
#include "tst_streamquartet.moc"
