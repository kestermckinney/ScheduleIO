// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPP14WRITER_H
#define MPP14WRITER_H

#include "model/project.h"

#include <QString>

class CompoundFile;

// Real MPP.14 (Project 2010-2021) writer.
//
// Strategy: start from an embedded template container (an empty project saved
// by Microsoft Project 16 — :/scheduleio/mpp14template.mpp), copy its storage
// tree verbatim (Props14, \1CompObj, the "   214" view/table/filter streams,
// the "   114" Props with its authentic field maps, ...), then regenerate the
// five Bknd entity storages (TBkndTask/TBkndRsc/TBkndAssn/TBkndCons/TBkndCal)
// from the model and patch the project-level dates and SummaryInformation.
//
// Every field is placed at the offset the template's own field map declares
// (parsed with the same FieldMap code the reader uses), so files we write are
// read back by our reader — and follow the exact record layouts recovered from
// real Project 2016 files (DECODING_NOTES.md) and the WINPROJ decompilation
// (VarMeta/FixedMeta headers: referenceapp.c FUN_1408af414 / FUN_1408af56c).
bool writeMpp14(const schedule::Project &in, CompoundFile &cf, QString *error);

#endif // MPP14WRITER_H
