// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPP14SERIALIZER_H
#define MPP14SERIALIZER_H

#include "serializer/docserializer.h"

// Binary MPP.14 (Project 2010+) serializer.
class Mpp14Serializer : public DocSerializer
{
public:
    MppProject::FormatVersion version() const override
    { return MppProject::FormatVersion::Mpp14; }
};

#endif // MPP14SERIALIZER_H
