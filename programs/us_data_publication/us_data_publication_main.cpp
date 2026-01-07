//! \file us_data_publication_main.cpp

#include <QApplication>
#include <QCommandLineParser>

#include "us_data_publication.h"
#include "us_license_t.h"
#include "us_license.h"

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName("us_data_publication");
    application.setApplicationVersion("1.0");

    // Check for CLI mode
    bool cliMode = false;
    for (int i = 1; i < argc; ++i) {
        QString arg = argv[i];
        if (arg == "--no-ui" || arg == "--mode" || arg == "--help" || arg == "-h") {
            cliMode = true;
            break;
        }
    }

    if (cliMode) {
        // Run in CLI mode
        return US_DataPublication::runCli(argc, argv);
    }

    // GUI mode - check license
    #include "main1.inc"

    // License is OK. Start up.
    US_DataPublication w;
    w.show();
    return application.exec();
}
