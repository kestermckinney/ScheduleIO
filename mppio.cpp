// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "mppio.h"

#include "src/ole/compoundfile.h"
#include "src/serializer/docserializer.h"

#include <QFile>

struct MppIO::Private {
    schedule::Project project;
    schedule::Project sourceProject;
    QByteArray sourceData;
    bool hasSource = false;
    QString error;
};

MppIO::MppIO() : d(new Private) {}
MppIO::~MppIO() { delete d; }

bool MppIO::open(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        d->error = QStringLiteral("cannot open '%1': %2").arg(path, f.errorString());
        return false;
    }
    return openFromData(f.readAll());
}

bool MppIO::openFromData(const QByteArray &bytes)
{
    d->error.clear();

    CompoundFile cf;
    if (!cf.openFromData(bytes)) {
        d->error = QStringLiteral("not a valid .mpp container: %1").arg(cf.errorString());
        return false;
    }

    const schedule::Project::FormatVersion v = DocSerializer::detectVersion(cf);
    auto ser = DocSerializer::create(v);
    if (!ser) {
        d->error = QStringLiteral("unsupported or unrecognised .mpp format version");
        return false;
    }

    schedule::Project parsed;
    if (!ser->read(cf, parsed, &d->error))
        return false;

    if (v == schedule::Project::FormatVersion::Mpp14)
        parsed.mppSourceTemplate = bytes;

    d->project = parsed;
    d->sourceProject = parsed;
    d->sourceData = bytes;
    d->hasSource = true;
    return true;
}

bool MppIO::save(const QString &path)
{
    const QByteArray bytes = saveToData();
    if (bytes.isEmpty()) {
        if (d->error.isEmpty())
            d->error = QStringLiteral("serialisation produced no data");
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        d->error = QStringLiteral("cannot write '%1': %2").arg(path, f.errorString());
        return false;
    }
    return f.write(bytes) == bytes.size();
}

QByteArray MppIO::saveToData()
{
    d->error.clear();

    // The semantic Project deliberately omits opaque streams and physical OLE
    // layout.  If a loaded document is still unchanged, returning its in-memory
    // source image is the only way to guarantee an exact binary round trip.
    if (d->hasSource && d->project == d->sourceProject)
        return d->sourceData;

    schedule::Project::FormatVersion v = d->project.formatVersion;
    if (v == schedule::Project::FormatVersion::Unknown)
        v = schedule::Project::FormatVersion::Mpp14;   // sensible default for new files

    auto ser = DocSerializer::create(v);
    if (!ser) {
        d->error = QStringLiteral("no serializer for the requested format version");
        return QByteArray();
    }

    CompoundFile cf;
    if (!ser->write(d->project, cf, &d->error))
        return QByteArray();
    return cf.toByteArray();
}

const schedule::Project &MppIO::project() const { return d->project; }
void MppIO::setProject(const schedule::Project &project)
{
    d->project = project;
    d->sourceProject = schedule::Project();
    d->sourceData.clear();
    d->hasSource = false;
}
QString MppIO::errorString() const { return d->error; }

// ---- C factory --------------------------------------------------------------
extern "C" {
MppIO *scheduleio_create() { return new MppIO(); }
void   scheduleio_destroy(MppIO *io) { delete io; }
const char *scheduleio_version() { return "0.1.0"; }
}
