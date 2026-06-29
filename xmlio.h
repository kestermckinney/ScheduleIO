// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef XMLIO_H
#define XMLIO_H

#include "mppio_export.h"
#include "src/model/mppproject.h"

#include <QString>

// Public facade for reading and writing Microsoft Project compatible XML
// (the MSPDI schema, the format MS Project exports/imports as .xml). It shares
// the exact same in-memory model as MppIO -- an MppProject of tasks, resources,
// assignments, calendars and links -- so a project can be loaded from a binary
// .mpp with MppIO and saved as .xml with XmlIO (or vice versa) without any
// translation step. Cross-platform and dynamically loadable; see the C factory
// entry points below.
class MPPIO_EXPORT XmlIO
{
public:
    XmlIO();
    ~XmlIO();

    // Read an MSPDI .xml file. Returns false and sets errorString() on failure.
    bool open(const QString &path);

    // Read from an in-memory XML image (handy for tests and embedded use).
    bool openFromData(const QByteArray &bytes);

    // Write the current project to an MSPDI .xml file / in-memory image.
    bool save(const QString &path);
    QByteArray saveToData();

    const MppProject &project() const;
    void setProject(const MppProject &project);

    QString errorString() const;

private:
    Q_DISABLE_COPY(XmlIO)
    struct Private;
    Private *d;
};

// ---- C factory for runtime (QLibrary / dlopen / LoadLibrary) loading --------
extern "C" {
MPPIO_EXPORT XmlIO *xmlio_create();
MPPIO_EXPORT void   xmlio_destroy(XmlIO *io);
MPPIO_EXPORT const char *xmlio_version();   // library version string, e.g. "0.1.0"
}

#endif // XMLIO_H
