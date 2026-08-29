include( ../../local.pri )

TEMPLATE = app
TARGET   = us_3dsa_cli
CONFIG  += console
CONFIG  -= app_bundle
QT      -= gui
QT      += core network xml sql

SOURCES  = us_3dsa_cli.cpp
HEADERS  = us_3dsa_cli.h
