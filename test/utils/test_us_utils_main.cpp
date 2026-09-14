// test_us_utils_main.cpp - Google Test
#include "qt_test_base.h"
#include <QCoreApplication>
#include <QApplication>

// Qt Test Environment for Google Test
class QtTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char* argv[] = {const_cast<char*>("test")};
            app_ = new QCoreApplication(argc, argv);
        }
        qputenv("QT_QPA_PLATFORM", "offscreen");

        // Optional: Suppress Qt debug output during tests
        qputenv("QT_LOGGING_RULES", "*.debug=false");
    }

    void TearDown() override {
        // Cleanup handled automatically
    }

private:
    QCoreApplication* app_ = nullptr;
};

int main(int argc, char **argv) {
    // Before anything reads or writes US_Settings: give this process its
    // own settings file, so parallel test processes do not overwrite each
    // other's scratch directory
    isolateTestSettings();

    // Initialize Google Test
    ::testing::InitGoogleTest(&argc, argv);

    // Add Qt environment
    ::testing::AddGlobalTestEnvironment(new QtTestEnvironment);

    // Run all tests - Google Test automatically discovers them!
    return RUN_ALL_TESTS();
}