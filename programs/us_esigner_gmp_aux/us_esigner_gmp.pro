include( ../../gui.pri )

TARGET        = us_esigner_gmp_aux
QT           += xml
QT           += sql
QT           += svg opengl
QT           += printsupport

HEADERS       = ../us_esigner_gmp/us_esigner_gmp.h                  

SOURCES       = us_esigner_gmp_main.cpp       \
                ../us_esigner_gmp/us_esigner_gmp.cpp



               
