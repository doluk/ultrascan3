// test/common/us_test_settings.h
#pragma once

/**
 * Give this process its own settings file.
 *
 * US_Settings reads and writes one QSettings file for the whole machine,
 * and a test that points UltraScan at a scratch store writes "workBaseDir"
 * into it.  ctest runs the cases in parallel processes, so without this
 * they overwrite each other's scratch directory and a test ends up reading
 * a store another one has just deleted.  It also keeps a test run from
 * touching the settings of whoever is running it.
 *
 * Call it once, from main, before anything reads US_Settings.
 */
void isolateTestSettings();
