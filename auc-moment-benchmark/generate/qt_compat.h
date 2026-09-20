// Build-time shim for building libus_utils against a Qt older than the one
// the project pins.
//
// UltraScan targets Qt 6.9 (the top-level CMakeLists sets
// QT_DISABLE_DEPRECATED_UP_TO=0x060900) and the upstream toolchain image
// supplies it.  Ubuntu 24.04 ships Qt 6.4.2.
//
// The only incompatibility that actually bites is comparing the QStringView
// returned by QXmlStreamReader::name()/QStringView generally against a plain
// string literal, which the utils XML readers do in ~146 places:
//
//     if ( xml.name() == "analyte" )
//
// Qt 6.5 gained the QStringView/const char* comparison; under 6.4 the
// compiler instead tries QChar's integral constructors and reports an
// ambiguity.  Supplying the operator here fixes every site at once WITHOUT
// editing a single upstream source file, which matters: patching 146
// call sites would fork the UltraScan sources in this branch.
//
// This header is force-included (-include) into the us_utils compilation
// only, and compiles to nothing on Qt >= 6.5.

#pragma once

#include <QtGlobal>

#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
#include <QLatin1String>
#include <QStringView>

inline bool operator==( QStringView v, const char* s )
{
   return v == QLatin1String( s );
}

inline bool operator!=( QStringView v, const char* s )
{
   return !( v == QLatin1String( s ) );
}
#endif
