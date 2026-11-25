us3_add_gui_test(test_us_editor_gui
        SOURCES
            test_us_editor_gui.cpp
        LIBRARIES
            UltraScan3::Gui_Static
        LABELS
            gui
        )

us3_add_gui_test(test_us_gui_test_isolation
        SOURCES
            test_us_gui_test_isolation.cpp
        LIBRARIES
            UltraScan3::Gui_Static
        LABELS
            gui
        )

us3_add_gui_test(test_us_select_runs_gui
        SOURCES
            test_us_select_runs_gui.cpp
        LIBRARIES
            UltraScan3::Gui_Static
        LABELS
            gui
        )

# The analysis programs' noise rule, which the data publication tests
# replicate (test/programs/us_data_publication/datapub_noise_rules.h)
us3_add_gui_test(test_us_loadable_noise_gui
        SOURCES
            test_us_loadable_noise_gui.cpp
        LIBRARIES
            UltraScan3::Gui_Static
        INCLUDE_DIRECTORIES
            ${CMAKE_SOURCE_DIR}/test/programs/us_data_publication
        LABELS
            gui
        )
