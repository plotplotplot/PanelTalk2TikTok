#pragma once

// Qt 6.4 + GCC 13 compatibility workaround
// The qtsan_impl.h header has issues when included before certain other headers.
// This wrapper ensures proper include order.

// Define this to skip TSAN instrumentation in Qt
#ifndef QT_NO_TSAN
#define QT_NO_TSAN
#endif

// Include the Qt global header first
#include <QtCore/qglobal.h>

// Now include other common Qt headers in the correct order
#include <QtCore/qnamespace.h>
