# Build against an UltraScan source tree:
#   qmake "US3=/path/to/ultrascan3" generate.pro && make
TEMPLATE = app
TARGET   = generate
CONFIG  += console c++17
CONFIG  -= app_bundle
QT      += core xml network sql
QT      -= gui

isEmpty(US3): US3 = $$PWD/../..
INCLUDEPATH += $$US3/utils
LIBS        += -L$$US3/lib -lus_utils

SOURCES += generate.cpp
