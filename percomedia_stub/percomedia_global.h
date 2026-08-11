#pragma once
#include <QtCore/qglobal.h>

#if defined(PERCOMEDIA_LIBRARY)
#  define PERCOMEDIA_EXPORT Q_DECL_EXPORT
#else
#  define PERCOMEDIA_EXPORT Q_DECL_IMPORT
#endif
