include( ../../gui.pri )

TARGET        = us_data_publication
QT           += xml

HEADERS       = us_data_publication.h \
                us_datapub_defs.h \
                us_datapub_manifest.h \
                us_datapub_hash.h \
                us_datapub_bundle.h \
                us_datapub_records.h \
                us_datapub_catalog.h \
                us_datapub_export.h \
                us_datapub_import.h \
                us_datapub_cli.h \
                us_datapub_export_pane.h \
                us_datapub_import_pane.h

SOURCES       = us_data_publication.cpp \
                us_datapub_defs.cpp \
                us_datapub_manifest.cpp \
                us_datapub_hash.cpp \
                us_datapub_bundle.cpp \
                us_datapub_records.cpp \
                us_datapub_catalog.cpp \
                us_datapub_export.cpp \
                us_datapub_import.cpp \
                us_datapub_cli.cpp \
                us_datapub_export_pane.cpp \
                us_datapub_import_pane.cpp
