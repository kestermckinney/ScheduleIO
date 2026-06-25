// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPIO_EXPORT_H
#define MPPIO_EXPORT_H

#include <QtCore/qglobal.h>

// MPPIO_DLL is defined for both consumers and the library when building shared.
// MPPIO_LIBRARY is defined only while compiling the library itself.
#if defined(MPPIO_DLL)
#  if defined(MPPIO_LIBRARY)
#    define MPPIO_EXPORT Q_DECL_EXPORT
#  else
#    define MPPIO_EXPORT Q_DECL_IMPORT
#  endif
#else
#  define MPPIO_EXPORT
#endif

#endif // MPPIO_EXPORT_H
