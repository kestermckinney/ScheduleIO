// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "xmlio.h"

#include "src/xml/xmlserializer.h"

#include <QFile>

struct XmlIO::Private {
    schedule::Project project;
    QString error;
};

XmlIO::XmlIO() : d(new Private) {}
XmlIO::~XmlIO() { delete d; }

bool XmlIO::open(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        d->error = QStringLiteral("cannot open '%1': %2").arg(path, f.errorString());
        return false;
    }
    return openFromData(f.readAll());
}

bool XmlIO::openFromData(const QByteArray &bytes)
{
    d->error.clear();
    schedule::Project parsed;
    if (!XmlSerializer::read(bytes, parsed, &d->error))
        return false;
    d->project = parsed;
    return true;
}

bool XmlIO::save(const QString &path)
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

QByteArray XmlIO::saveToData()
{
    d->error.clear();
    return XmlSerializer::write(d->project, &d->error);
}

const schedule::Project &XmlIO::project() const { return d->project; }
void XmlIO::setProject(const schedule::Project &project) { d->project = project; }
QString XmlIO::errorString() const { return d->error; }

// ---- C factory --------------------------------------------------------------
extern "C" {
XmlIO *xmlio_create() { return new XmlIO(); }
void   xmlio_destroy(XmlIO *io) { delete io; }
const char *xmlio_version() { return "0.1.0"; }
}
