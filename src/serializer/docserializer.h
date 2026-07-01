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
// SCAFFOLD STATUS: read()/write() implement a self-consistent quartet-based
// encoding so the whole open/save pipeline and its tests work end to end. The
// exact on-disk MPP field packing (FixedData record layouts, field type ids)
// is the reverse-engineering work to be done against the user's real fixtures
// (plan, Layers 2-3). The seam to fill is marked in docserializer.cpp.
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
