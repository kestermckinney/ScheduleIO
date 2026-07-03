// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef DOCSERIALIZER_H
#define DOCSERIALIZER_H

#include "model/project.h"

#include <QString>
#include <memory>

class CompoundFile;

// Maps the project object model to/from the CFB storage tree. Concrete
// subclasses correspond to the WINPROJ serializer classes CPSI12DocSer (.12)
// and the .14 serializer.
//
// read() parses real Microsoft Project files (the "   114" Bknd storages).
// write() emits the real MPP.14 format for FormatVersion::Mpp14 (template-based
// writer, mpp14writer.cpp — validated by round-trip tests and by MPXJ reading
// our output). MPP.12 write still uses the legacy scaffold container, which
// only this library can read back.
class DocSerializer
{
public:
    virtual ~DocSerializer() = default;

    // Inspect the container and decide which format it is.
    static schedule::Project::FormatVersion detectVersion(const CompoundFile &cf);
    static std::unique_ptr<DocSerializer> create(schedule::Project::FormatVersion v);

    virtual schedule::Project::FormatVersion version() const = 0;

    bool read(const CompoundFile &cf, schedule::Project &out, QString *error) const;
    bool write(const schedule::Project &in, CompoundFile &cf, QString *error) const;
};

#endif // DOCSERIALIZER_H
