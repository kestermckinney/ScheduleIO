// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULEIO_EXPORT_H
#define SCHEDULEIO_EXPORT_H

#include <QtCore/qglobal.h>

// SCHEDULEIO_DLL is defined for both consumers and the library when building shared.
// SCHEDULEIO_LIBRARY is defined only while compiling the library itself.
#if defined(SCHEDULEIO_DLL)
#  if defined(SCHEDULEIO_LIBRARY)
#    define SCHEDULEIO_EXPORT Q_DECL_EXPORT
#  else
#    define SCHEDULEIO_EXPORT Q_DECL_IMPORT
#  endif
#else
#  define SCHEDULEIO_EXPORT
#endif

#endif // SCHEDULEIO_EXPORT_H
