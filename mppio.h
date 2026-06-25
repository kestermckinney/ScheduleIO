// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPIO_H
#define MPPIO_H

#include "mppio_export.h"
#include "src/model/mppproject.h"

#include <QString>

// Public facade for the MppIO library. Reads a Microsoft Project .mpp file into
// an MppProject (Qt data structures) and writes one back out. Cross-platform
// (Windows/macOS/Linux) and intended to be dynamically loaded -- see the C
// factory entry points below for QLibrary/dlopen-based runtime use.
class MPPIO_EXPORT MppIO
{
public:
    MppIO();
    ~MppIO();

    // Read a .mpp file. Returns false and sets errorString() on failure.
    bool open(const QString &path);

    // Read from an in-memory .mpp image (handy for tests and embedded use).
    bool openFromData(const QByteArray &bytes);

    // Write the current project to a .mpp file / in-memory image.
    bool save(const QString &path);
    QByteArray saveToData();

    const MppProject &project() const;
    void setProject(const MppProject &project);

    QString errorString() const;

private:
    Q_DISABLE_COPY(MppIO)
    struct Private;
    Private *d;
};

// ---- C factory for runtime (QLibrary / dlopen / LoadLibrary) loading --------
extern "C" {
MPPIO_EXPORT MppIO *mppio_create();
MPPIO_EXPORT void   mppio_destroy(MppIO *io);
MPPIO_EXPORT const char *mppio_version();   // library version string, e.g. "0.1.0"
}

#endif // MPPIO_H
