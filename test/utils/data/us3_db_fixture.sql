-- us3_db_fixture.sql - a small data set for the database integration tests
--
-- Load it into a scratch database that already has the UltraScan3 schema and
-- stored procedures (us3_sql), then point test_us_utils_db at it:
--
--   mysql -u root us3test < us3_schema.sql
--   mysql -u root us3test < us3_procedures.sql
--   mysql -u root us3test < test/utils/data/us3_db_fixture.sql
--
--   US3_TEST_DB_HOST=127.0.0.1:3306 US3_TEST_DB_NAME=us3test \
--   US3_TEST_DB_USER=us3test        US3_TEST_DB_PASS=us3testpw \
--   US3_TEST_DB_PERSON_GUID=aaaaaaaa-0000-0000-0000-00000000user \
--   US3_TEST_DB_PERSON_PW=secret    US3_TEST_DB_PERSON_ID=2 \
--   US3_TEST_DB_EMAIL=test@example.org  ./bin/test_us_utils_db
--
--   US3_TEST_DB_ADMIN_GUID=aaaaaaaa-0000-0000-0000-0000000admin \
--   US3_TEST_DB_ADMIN_PW=adminpw  US3_TEST_DB_OTHER_ID=3
--
-- The investigator here is deliberately an ordinary user, not an admin, so
-- the tests go through the same permission checks a real user does.  The
-- administrator at the foot of this file is for the tests that switch
-- between investigators, which no ordinary user is allowed to do.

-- an ordinary user: userlevel 2 is below @US3_ADMIN
INSERT INTO people (personID, personGUID, fname, lname, address, city, state, zip,
                    phone, email, organization, password, activated, signup,
                    lastLogin, userlevel, account_enabled, authenticatePAM, userNamePAM)
VALUES (2, 'aaaaaaaa-0000-0000-0000-00000000user', 'Test', 'User', 'x','x','TX','00000',
        '000', 'test@example.org', 'Lab', MD5('secret'), 1, NOW(), NOW(), 2, 1, 0, '');

INSERT INTO project (projectID, projectGUID, goals, molecules, purity, expense,
                     bufferComponents, saltInformation, AUC_questions, expDesign,
                     notes, description, status)
VALUES (5, 'aaaaaaaa-0000-0000-0000-0000000proj', 'g','m','95','e','b','s','q','d','n',
        'MWL demo project', 'submitted');
INSERT INTO projectPerson (projectID, personID) VALUES (5, 2);

INSERT IGNORE INTO lab (labID, labGUID, name, building, room) VALUES (1,'lab-guid','Lab','B','R');
INSERT IGNORE INTO instrument (instrumentID, name, serialNumber, labID) VALUES (1,'XLA','SN1',1);

INSERT INTO experiment (experimentID, projectID, runID, labID, instrumentID, operatorID,
                        rotorID, rotorCalibrationID, experimentGUID, type, runType,
                        dateBegin, runTemp, label, comment)
VALUES (10, 5, 'MWL_demo_run', 1, 1, 2, NULL, NULL,
        'aaaaaaaa-0000-0000-0000-00000000exp1', 'velocity', 'RA', CURDATE(), 20.0,
        'MWL demo', 'c');
INSERT INTO experimentPerson (experimentID, personID) VALUES (10, 2);

-- a second experiment with nothing under it, to prove the counts are per row
INSERT INTO experiment (experimentID, projectID, runID, labID, instrumentID, operatorID,
                        rotorID, rotorCalibrationID, experimentGUID, type, runType,
                        dateBegin, runTemp, label, comment)
VALUES (11, 5, 'Empty_run', 1, 1, 2, NULL, NULL,
        'aaaaaaaa-0000-0000-0000-00000000exp2', 'velocity', 'RA', CURDATE(), 20.0,
        'empty', 'c');
INSERT INTO experimentPerson (experimentID, personID) VALUES (11, 2);

INSERT IGNORE INTO channel (channelID, abstractChannelID, channelGUID) VALUES (1, NULL, 'chan-guid');

INSERT INTO solution (solutionID, solutionGUID, description, commonVbar20, storageTemp, notes)
VALUES (7, 'aaaaaaaa-0000-0000-0000-00000000sol1', 'Demo solution', 0.72, 20, '');

-- three triples, two edits on the first, one model and one noise on that edit
INSERT INTO rawData (rawDataID, rawDataGUID, label, filename, data, comment,
                     experimentID, solutionID, channelID)
VALUES (100,'aaaaaaaa-0000-0000-0000-00000000raw1','MWL_demo_run','MWL_demo_run.RA.1.A.280.auc',
        'RAWBYTES-1','c',10,7,1),
       (101,'aaaaaaaa-0000-0000-0000-00000000raw2','MWL_demo_run','MWL_demo_run.RA.1.A.281.auc',
        'RAWBYTES-2','c',10,7,1),
       (102,'aaaaaaaa-0000-0000-0000-00000000raw3','MWL_demo_run','MWL_demo_run.RA.1.A.282.auc',
        'RAWBYTES-3','c',10,7,1);

INSERT INTO editedData (editedDataID, rawDataID, editGUID, label, data, filename, comment)
VALUES (200,100,'aaaaaaaa-0000-0000-0000-0000000edit1','MWL_demo_run','EDITBYTES-1',
        'MWL_demo_run.2401011200.RA.1.A.280.xml','c'),
       (201,100,'aaaaaaaa-0000-0000-0000-0000000edit2','MWL_demo_run','EDITBYTES-2',
        'MWL_demo_run.2401011300.RA.1.A.280.xml','c'),
       (202,101,'aaaaaaaa-0000-0000-0000-0000000edit3','MWL_demo_run','EDITBYTES-3',
        'MWL_demo_run.2401011200.RA.1.A.281.xml','c');

INSERT INTO model (modelID, editedDataID, modelGUID, meniscus, MCIteration, variance,
                   description, xml, globalType)
VALUES (300, 200, 'aaaaaaaa-0000-0000-0000-000000model1', 5.85, 1, 0.001,
        'MWL_demo_run.1A280.2dsa.model', '<model/>', 'NORMAL');
INSERT INTO modelPerson (modelID, personID) VALUES (300, 2);

INSERT INTO noise (noiseID, noiseGUID, editedDataID, modelID, modelGUID, noiseType,
                   description, xml)
VALUES (400, 'aaaaaaaa-0000-0000-0000-000000noise1', 200, 300,
        'aaaaaaaa-0000-0000-0000-000000model1', 'ti_noise',
        'MWL_demo_run.1A280.2dsa.ti_noise', '<noise/>');

-- ----------------------------------------------------------------------
-- A second investigator, and an administrator who can read both of them.
--
-- The investigator is chosen in the GUI and goes into every catalog query
-- as a parameter, while the login stays the same, so switching between
-- these two is what the tests about a changed investigator need.  Only an
-- administrator may ask for another person's records, which is why one is
-- here; the ordinary user above still exercises the permission checks.
-- ----------------------------------------------------------------------
INSERT INTO people (personID, personGUID, fname, lname, address, city, state, zip,
                    phone, email, organization, password, activated, signup,
                    lastLogin, userlevel, account_enabled, authenticatePAM, userNamePAM)
VALUES (1, 'aaaaaaaa-0000-0000-0000-0000000admin', 'Test', 'Admin', 'x','x','TX','00000',
        '000', 'admin@example.org', 'Lab', MD5('adminpw'), 1, NOW(), NOW(), 3, 1, 0,
        'test_admin'),
       (3, 'aaaaaaaa-0000-0000-0000-0000000user3', 'Other', 'User', 'x','x','TX','00000',
        '000', 'other@example.org', 'Lab', MD5('secret3'), 1, NOW(), NOW(), 2, 1, 0,
        'test_other');

INSERT INTO project (projectID, projectGUID, goals, molecules, purity, expense,
                     bufferComponents, saltInformation, AUC_questions, expDesign,
                     notes, description, status)
VALUES (6, 'aaaaaaaa-0000-0000-0000-000000proj2', 'g','m','95','e','b','s','q','d','n',
        'Other person project', 'submitted');
INSERT INTO projectPerson (projectID, personID) VALUES (6, 3);

INSERT INTO experiment (experimentID, projectID, runID, labID, instrumentID, operatorID,
                        rotorID, rotorCalibrationID, experimentGUID, type, runType,
                        dateBegin, runTemp, label, comment)
VALUES (12, 6, 'Other_run', 1, 1, 3, NULL, NULL,
        'aaaaaaaa-0000-0000-0000-00000000exp3', 'velocity', 'RA', CURDATE(), 20.0,
        'other', 'c');
INSERT INTO experimentPerson (experimentID, personID) VALUES (12, 3);

INSERT INTO rawData (rawDataID, rawDataGUID, label, filename, data, comment,
                     experimentID, solutionID, channelID)
VALUES (110,'aaaaaaaa-0000-0000-0000-00000000raw4','Other_run','Other_run.RA.1.A.280.auc',
        'RAWBYTES-4','c',12,7,1);

INSERT INTO editedData (editedDataID, rawDataID, editGUID, label, data, filename, comment)
VALUES (210,110,'aaaaaaaa-0000-0000-0000-0000000edit4','Other_run','EDITBYTES-4',
        'Other_run.2401011200.RA.1.A.280.xml','c');
