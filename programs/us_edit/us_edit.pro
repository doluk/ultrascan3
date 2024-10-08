include( ../../gui.pri )

TARGET        = us_edit
QT           += xml

HEADERS       = us_edit.h            \
                us_exclude_profile.h \
                us_get_edit.h        \
                us_ri_noise.h        \
                us_edit_scan.h       \
                us_select_lambdas.h

SOURCES       = us_edit_main.cpp       \
                us_edit.cpp            \
                us_exclude_profile.cpp \
                us_get_edit.cpp        \
                us_ri_noise.cpp        \
                us_edit_scan.cpp       \
                us_select_lambdas.cpp

