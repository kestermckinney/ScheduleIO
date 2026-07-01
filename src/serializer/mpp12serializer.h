// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPP12SERIALIZER_H
#define MPP12SERIALIZER_H

#include "serializer/docserializer.h"

// Binary MPP.12 (Project 2007) serializer -- corresponds to WINPROJ's
// CPSI12DocSer (referenceapp.c vaddr 0x1667276).
class Mpp12Serializer : public DocSerializer
{
public:
    schedule::Project::FormatVersion version() const override
    { return schedule::Project::FormatVersion::Mpp12; }
};

#endif // MPP12SERIALIZER_H
