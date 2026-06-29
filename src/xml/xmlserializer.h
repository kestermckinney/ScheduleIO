// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef XMLSERIALIZER_H
#define XMLSERIALIZER_H

#include "model/mppproject.h"

#include <QByteArray>
#include <QString>

// Reads and writes a Microsoft Project compatible XML file (the MSPDI schema,
// http://schemas.microsoft.com/project) to and from the shared MppProject object
// model -- the very same data structure the binary .mpp reader/writer use. This
// is the engine behind the XmlIO facade; it is internal to the library (not part
// of the exported ABI) so the tests can drive it directly.
namespace XmlSerializer {

// Parse an MSPDI document into `out`. Returns false and fills `error` on failure.
bool read(const QByteArray &xml, MppProject &out, QString *error);

// Serialise `in` to a UTF-8 MSPDI document. Returns an empty array and fills
// `error` only on a genuine failure (an empty project still produces a document).
QByteArray write(const MppProject &in, QString *error);

} // namespace XmlSerializer

#endif // XMLSERIALIZER_H
