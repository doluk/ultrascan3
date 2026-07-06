//! \file us_data_publication_main.cpp

#include <QCoreApplication>
#include <QApplication>
#include <QCommandLineParser>

#include "us_data_publication.h"
#include "us_license_t.h"
#include "us_license.h"

int main(int argc, char* argv[])
{
    // Detect CLI mode before creating any QApplication, so we can use
    // QCoreApplication (which works in headless environments) instead of
    // QApplication (which requires a display/platform plugin).
    bool cliMode = false;
    for (int i = 1; i < argc; ++i) {
        QString arg = argv[i];
        if (arg == "--no-ui" || arg == "--mode" || arg == "--help" || arg == "-h") {
            cliMode = true;
            break;
        }
    }

    if (cliMode) {
        // Run in CLI mode without a display
        QCoreApplication application(argc, argv);
        application.setApplicationName("us_data_publication");
        application.setApplicationVersion("1.0");
        return US_DataPublication::runCli(argc, argv);
    }

    // GUI mode - requires a full QApplication with widget support
    QApplication application(argc, argv);
    application.setApplicationName("us_data_publication");
    application.setApplicationVersion("1.0");

    // Check license
    #include "main1.inc"

    // License is OK. Start up.
    US_DataPublication w;
    w.show();
    return application.exec();
}
