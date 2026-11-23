-- us3_db_fixture.sql - a small, complete data set for the database integration tests
--
-- Load it into a scratch database that already has the UltraScan3 schema,
-- stored procedures and hardware data (us3_sql: us3.sql, us3_procedures.sql,
-- us3_hardware_data.sql, us3_buffer_components.sql), then point the tests at
-- it:
--
--   mysql -u root us3test < test/utils/data/us3_db_fixture.sql
--
--   US3_TEST_DB_HOST=127.0.0.1:3306 US3_TEST_DB_NAME=us3test \
--   US3_TEST_DB_USER=us3test        US3_TEST_DB_PASS=us3testpw \
--   US3_TEST_DB_PERSON_GUID=aaaaaaaa-0000-4000-8000-000000000002 \
--   US3_TEST_DB_PERSON_PW=secret    US3_TEST_DB_PERSON_ID=2 \
--   US3_TEST_DB_EMAIL=test@example.org  ./bin/test_us_utils_db
--
--   US3_TEST_DB_ADMIN_GUID=aaaaaaaa-0000-4000-8000-000000000001 \
--   US3_TEST_DB_ADMIN_PW=adminpw  US3_TEST_DB_OTHER_ID=3
--
-- The investigator here is deliberately an ordinary user, not an admin, so
-- the tests go through the same permission checks a real user does.  The
-- administrator at the foot of this file is for the tests that switch
-- between investigators, which no ordinary user is allowed to do.
--
-- Every record is what UltraScan itself would have written, so that a run
-- can be read and exported whole: every GUID is a real UUID (an export
-- refuses anything else, as an import does), the raw data are real .auc
-- files carrying their own GUID, the edits real edit XML naming their edit
-- and raw data, the model and noise real US_Model / US_Noise XML; a
-- solution has its person, buffer (with components and an extinction
-- profile) and analyte (with an extinction profile), and is associated with
-- the experiment; and the run has a rotor, a calibration and a cell table.
-- The payloads were written by US_DataIO::writeRawData(), US_Model::write()
-- and US_Noise::write().  The empty run has no rotor (a NULL rotorID, as in
-- old records), which a strict server must not be asked about with ''.

-- an ordinary user: userlevel 2 is below @US3_ADMIN
INSERT INTO people (personID, personGUID, fname, lname, address, city, state, zip,
                    phone, email, organization, password, activated, signup,
                    lastLogin, userlevel, account_enabled, authenticatePAM, userNamePAM)
VALUES (2, 'aaaaaaaa-0000-4000-8000-000000000002', 'Test', 'User', 'x','x','TX','00000',
        '000', 'test@example.org', 'Lab', MD5('secret'), 1, NOW(), NOW(), 2, 1, 0, '');

INSERT INTO project (projectID, projectGUID, goals, molecules, purity, expense,
                     bufferComponents, saltInformation, AUC_questions, expDesign,
                     notes, description, status)
VALUES (5, 'aaaaaaaa-0000-4000-8000-000000000005', 'g','m','95','e','b','s','q','d','n',
        'MWL demo project', 'submitted');
INSERT INTO projectPerson (projectID, personID) VALUES (5, 2);

-- The hardware data already has lab 1; an instrument in it
INSERT IGNORE INTO lab (labID, labGUID, name, building, room)
VALUES (1, 'aaaaaaaa-0000-4000-8000-000000000901', 'Lab', 'B', 'R');
INSERT IGNORE INTO instrument (instrumentID, name, serialNumber, labID, opsys1, opsys2, opsys3)
VALUES (1, 'XLA', 'SN1', 1, 'UV/visible', 'UV/visible', 'UV/visible');

-- the run, made on rotor 1 with its calibration 1 (both from the hardware data)
INSERT INTO experiment (experimentID, projectID, runID, labID, instrumentID, operatorID,
                        rotorID, rotorCalibrationID, experimentGUID, type, runType,
                        dateBegin, runTemp, label, comment)
VALUES (10, 5, 'MWL_demo_run', 1, 1, 2, 1, 1,
        'aaaaaaaa-0000-4000-8000-000000000010', 'velocity', 'RA', CURDATE(), 20.0,
        'MWL demo', 'c');
INSERT INTO experimentPerson (experimentID, personID) VALUES (10, 2);

-- a second experiment with nothing under it, to prove the counts are per
-- row; an old record that names no rotor or calibration
INSERT INTO experiment (experimentID, projectID, runID, labID, instrumentID, operatorID,
                        rotorID, rotorCalibrationID, experimentGUID, type, runType,
                        dateBegin, runTemp, label, comment)
VALUES (11, 5, 'Empty_run', 1, 1, 2, NULL, NULL,
        'aaaaaaaa-0000-4000-8000-000000000011', 'velocity', 'RA', CURDATE(), 20.0,
        'empty', 'c');
INSERT INTO experimentPerson (experimentID, personID) VALUES (11, 2);

INSERT IGNORE INTO channel (channelID, abstractChannelID, channelGUID)
VALUES (1, NULL, 'aaaaaaaa-0000-4000-8000-000000000902');

-- cell 1, channel A of the run holds an Epon 2-channel centerpiece; holeNumber
-- is the index of the channel in US_Util::channel_letters() ("SABCDEFGH"), so
-- channel A is 1, as US_ConvertIO writes it
INSERT INTO cell (cellID, cellType, abstractCenterpieceID, experimentID, cellGUID, name,
                  holeNumber, dateUpdated)
VALUES (60, 'sample', 2, 10, 'aaaaaaaa-0000-4000-8000-000000000060', '1', 1, NOW());

-- The buffer: 150 mM sodium chloride and 10 mM Tris (component IDs of the
-- hardware data's component table), with an extinction profile
INSERT INTO buffer (bufferID, bufferGUID, description, compressibility, pH, viscosity,
                    density, manual)
VALUES (20, 'aaaaaaaa-0000-4000-8000-000000000020', 'Demo buffer', 4.6e-10, 7.4, 1.0185, 1.0057, 0);
INSERT INTO bufferPerson (bufferID, personID, private) VALUES (20, 2, 0);
INSERT INTO bufferLink (bufferID, bufferComponentID, concentration)
VALUES (20, 70, 150.0), (20, 92, 10.0);
INSERT INTO extinctionProfile (profileID, componentID, componentType, valueType, xml)
VALUES (1, 20, 'Buffer', 'molarExtinction', '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE US_EProfile>\n<EProfileData version="1.0"><extinctionProfile componentID="20" componentType="Buffer" valueType="molarExtinction"><extinction wavelength="260" value="0.0123456789"/><extinction wavelength="280" value="0.00987654321"/></extinctionProfile></EProfileData>\n');

-- The analyte: a protein, with an extinction profile
INSERT INTO analyte (analyteID, analyteGUID, type, sequence, vbar, description, spectrum,
                     molecularWeight)
VALUES (30, 'aaaaaaaa-0000-4000-8000-000000000030', 'Protein', '', 0.7334, 'Demo analyte', '', 66432.9);
INSERT INTO analytePerson (analyteID, personID) VALUES (30, 2);
INSERT INTO extinctionProfile (profileID, componentID, componentType, valueType, xml)
VALUES (2, 30, 'Analyte', 'molarExtinction', '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE US_EProfile>\n<EProfileData version="1.0"><extinctionProfile componentID="30" componentType="Analyte" valueType="molarExtinction"><extinction wavelength="230" value="254000.123456"/><extinction wavelength="280" value="43824.1234567"/></extinctionProfile></EProfileData>\n');

-- The solution in that channel: the buffer, the analyte, and a profile of its own
INSERT INTO solution (solutionID, solutionGUID, description, commonVbar20, storageTemp, notes)
VALUES (7, 'aaaaaaaa-0000-4000-8000-000000000007', 'Demo solution', 0.7334, 20, '');
INSERT INTO solutionPerson (solutionID, personID) VALUES (7, 2);
INSERT INTO solutionBuffer (solutionID, bufferID) VALUES (7, 20);
INSERT INTO solutionAnalyte (solutionID, analyteID, amount) VALUES (7, 30, 1.0);
INSERT INTO experimentSolutionChannel (experimentID, solutionID, channelID) VALUES (10, 7, 1);
INSERT INTO extinctionProfile (profileID, componentID, componentType, valueType, xml)
VALUES (3, 7, 'Sample', 'molarExtinction', '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE US_EProfile>\n<EProfileData version="1.0"><extinctionProfile componentID="7" componentType="Sample" valueType="molarExtinction"><extinction wavelength="280" value="43824.987654321"/></extinctionProfile></EProfileData>\n');

-- three triples, two edits on the first, one model and one noise on that edit
INSERT INTO rawData (rawDataID, rawDataGUID, label, filename, data, comment,
                     experimentID, solutionID, channelID)
VALUES (100,'aaaaaaaa-0000-4000-8000-000000000100','MWL_demo_run','MWL_demo_run.RA.1.A.280.auc',
        X'55434441303552410141AAAAAAAA0000400080000000000001004D574C2064656D6F20747269706C650000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000009A99B9406666BE400AD7233CCDCC4C3DCDCC4C3F00000000000000000100444154410000A04100C82F47E8030000F9021550F00A0AD7233C100000000000111122223333444455556666777788889999AAAABBBBCCCCDDDDEEEEFFFF0000728CC4A4','c',10,7,1),
       (101,'aaaaaaaa-0000-4000-8000-000000000101','MWL_demo_run','MWL_demo_run.RA.1.A.281.auc',
        X'55434441303552410141AAAAAAAA0000400080000000000001014D574C2064656D6F20747269706C650000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000009A99B9406666BE400AD7233CCDCC4C3DCDCC4C3F00000000000000000100444154410000A04100C82F47E8030000F9021550FA0A0AD7233C100000000000111122223333444455556666777788889999AAAABBBBCCCCDDDDEEEEFFFF0000A7BE6381','c',10,7,1),
       (102,'aaaaaaaa-0000-4000-8000-000000000102','MWL_demo_run','MWL_demo_run.RA.1.A.282.auc',
        X'55434441303552410141AAAAAAAA0000400080000000000001024D574C2064656D6F20747269706C650000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000009A99B9406666BE400AD7233CCDCC4C3DCDCC4C3F00000000000000000100444154410000A04100C82F47E8030000F9021550040B0AD7233C100000000000111122223333444455556666777788889999AAAABBBBCCCCDDDDEEEEFFFF0000F74758A9','c',10,7,1);

INSERT INTO editedData (editedDataID, rawDataID, editGUID, label, data, filename, comment)
VALUES (200,100,'aaaaaaaa-0000-4000-8000-000000000200','MWL_demo_run','<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE UltraScanEdits>\n<experiment type="velocity">\n    <editGUID value="aaaaaaaa-0000-4000-8000-000000000200"/>\n    <rawDataGUID value="aaaaaaaa-0000-4000-8000-000000000100"/>\n    <run cell="1" channel="A" wavelength="280">\n        <parameters meniscus="5.85" dataStart="5.9" dataEnd="5.95" plateau="5.93" baseline="5.86"/>\n    </run>\n</experiment>\n',
        'MWL_demo_run.2401011200.RA.1.A.280.xml','c'),
       (201,100,'aaaaaaaa-0000-4000-8000-000000000201','MWL_demo_run','<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE UltraScanEdits>\n<experiment type="velocity">\n    <editGUID value="aaaaaaaa-0000-4000-8000-000000000201"/>\n    <rawDataGUID value="aaaaaaaa-0000-4000-8000-000000000100"/>\n    <run cell="1" channel="A" wavelength="280">\n        <parameters meniscus="5.85" dataStart="5.9" dataEnd="5.95" plateau="5.93" baseline="5.86"/>\n    </run>\n</experiment>\n',
        'MWL_demo_run.2401011300.RA.1.A.280.xml','c'),
       (202,101,'aaaaaaaa-0000-4000-8000-000000000202','MWL_demo_run','<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE UltraScanEdits>\n<experiment type="velocity">\n    <editGUID value="aaaaaaaa-0000-4000-8000-000000000202"/>\n    <rawDataGUID value="aaaaaaaa-0000-4000-8000-000000000101"/>\n    <run cell="1" channel="A" wavelength="281">\n        <parameters meniscus="5.85" dataStart="5.9" dataEnd="5.95" plateau="5.93" baseline="5.86"/>\n    </run>\n</experiment>\n',
        'MWL_demo_run.2401011200.RA.1.A.281.xml','c');

INSERT INTO model (modelID, editedDataID, modelGUID, meniscus, MCIteration, variance,
                   description, xml, globalType)
VALUES (300, 200, 'aaaaaaaa-0000-4000-8000-000000000300', 5.85, 1, 0.001,
        'MWL_demo_run.1A280.2dsa.model', '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE US_Model>\n<ModelData version="1.0">\n    <model description="MWL_demo_run.1A280.2dsa.model" modelGUID="aaaaaaaa-0000-4000-8000-000000000300" editGUID="aaaaaaaa-0000-4000-8000-000000000200" wavelength="0" coSedSolute="-1" opticsType="0" analysisType="1" globalType="0">\n        <analyte name="New Component" mw="66000" s="4.5e-13" D="6.2e-07" f="1" f_f0="1.25" vbar20="0.72" extinction="0" axial="10" sigma="0" delta="0" oligomer="1" shape="0" type="0" molar="0" signal="0.6"/>\n    </model>\n</ModelData>\n', 'NORMAL');
INSERT INTO modelPerson (modelID, personID) VALUES (300, 2);

INSERT INTO noise (noiseID, noiseGUID, editedDataID, modelID, modelGUID, noiseType,
                   description, xml)
VALUES (400, 'aaaaaaaa-0000-4000-8000-000000000400', 200, 300,
        'aaaaaaaa-0000-4000-8000-000000000300', 'ti_noise',
        'MWL_demo_run.1A280.2dsa.ti_noise', '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE US_Noise>\n<NoiseData version="1.0">\n    <noise type="ti" description="MWL_demo_run.1A280.2dsa.ti_noise" modelGUID="aaaaaaaa-0000-4000-8000-000000000300" noiseGUID="aaaaaaaa-0000-4000-8000-000000000400" minradius="5.8" maxradius="5.95">\n        <d v="0"/>\n        <d v="0.001"/>\n        <d v="0.002"/>\n        <d v="0.003"/>\n        <d v="0.004"/>\n        <d v="0.005"/>\n        <d v="0.006"/>\n        <d v="0.007"/>\n        <d v="0.008"/>\n        <d v="0.009"/>\n        <d v="0.01"/>\n        <d v="0.011"/>\n        <d v="0.012"/>\n        <d v="0.013"/>\n        <d v="0.014"/>\n        <d v="0.015"/>\n    </noise>\n</NoiseData>\n');

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
VALUES (1, 'aaaaaaaa-0000-4000-8000-000000000001', 'Test', 'Admin', 'x','x','TX','00000',
        '000', 'admin@example.org', 'Lab', MD5('adminpw'), 1, NOW(), NOW(), 3, 1, 0,
        'test_admin'),
       (3, 'aaaaaaaa-0000-4000-8000-000000000003', 'Other', 'User', 'x','x','TX','00000',
        '000', 'other@example.org', 'Lab', MD5('secret3'), 1, NOW(), NOW(), 2, 1, 0,
        'test_other');

INSERT INTO project (projectID, projectGUID, goals, molecules, purity, expense,
                     bufferComponents, saltInformation, AUC_questions, expDesign,
                     notes, description, status)
VALUES (6, 'aaaaaaaa-0000-4000-8000-000000000006', 'g','m','95','e','b','s','q','d','n',
        'Other person project', 'submitted');
INSERT INTO projectPerson (projectID, personID) VALUES (6, 3);

INSERT INTO experiment (experimentID, projectID, runID, labID, instrumentID, operatorID,
                        rotorID, rotorCalibrationID, experimentGUID, type, runType,
                        dateBegin, runTemp, label, comment)
VALUES (12, 6, 'Other_run', 1, 1, 3, 1, 1,
        'aaaaaaaa-0000-4000-8000-000000000012', 'velocity', 'RA', CURDATE(), 20.0,
        'other', 'c');
INSERT INTO experimentPerson (experimentID, personID) VALUES (12, 3);

INSERT INTO cell (cellID, cellType, abstractCenterpieceID, experimentID, cellGUID, name,
                  holeNumber, dateUpdated)
VALUES (61, 'sample', 2, 12, 'aaaaaaaa-0000-4000-8000-000000000061', '1', 1, NOW());

-- the other investigator's own buffer, analyte and solution
INSERT INTO buffer (bufferID, bufferGUID, description, compressibility, pH, viscosity,
                    density, manual)
VALUES (21, 'aaaaaaaa-0000-4000-8000-000000000021', 'Other buffer', 4.6e-10, 7.0, 1.002, 0.9982, 0);
INSERT INTO bufferPerson (bufferID, personID, private) VALUES (21, 3, 1);
INSERT INTO analyte (analyteID, analyteGUID, type, sequence, vbar, description, spectrum,
                     molecularWeight)
VALUES (31, 'aaaaaaaa-0000-4000-8000-000000000031', 'Protein', '', 0.73, 'Other analyte', '', 50000);
INSERT INTO analytePerson (analyteID, personID) VALUES (31, 3);
INSERT INTO solution (solutionID, solutionGUID, description, commonVbar20, storageTemp, notes)
VALUES (8, 'aaaaaaaa-0000-4000-8000-000000000008', 'Other solution', 0.73, 20, '');
INSERT INTO solutionPerson (solutionID, personID) VALUES (8, 3);
INSERT INTO solutionBuffer (solutionID, bufferID) VALUES (8, 21);
INSERT INTO solutionAnalyte (solutionID, analyteID, amount) VALUES (8, 31, 0.5);
INSERT INTO experimentSolutionChannel (experimentID, solutionID, channelID) VALUES (12, 8, 1);

INSERT INTO rawData (rawDataID, rawDataGUID, label, filename, data, comment,
                     experimentID, solutionID, channelID)
VALUES (110,'aaaaaaaa-0000-4000-8000-000000000110','Other_run','Other_run.RA.1.A.280.auc',
        X'55434441303552410141AAAAAAAA0000400080000000000001104D574C2064656D6F20747269706C650000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000009A99B9406666BE400AD7233CCDCC4C3DCDCC4C3F00000000000000000100444154410000A04100C82F47E8030000F9021550F00A0AD7233C100000000000111122223333444455556666777788889999AAAABBBBCCCCDDDDEEEEFFFF00003BCC4518','c',12,8,1);

INSERT INTO editedData (editedDataID, rawDataID, editGUID, label, data, filename, comment)
VALUES (210,110,'aaaaaaaa-0000-4000-8000-000000000210','Other_run','<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE UltraScanEdits>\n<experiment type="velocity">\n    <editGUID value="aaaaaaaa-0000-4000-8000-000000000210"/>\n    <rawDataGUID value="aaaaaaaa-0000-4000-8000-000000000110"/>\n    <run cell="1" channel="A" wavelength="280">\n        <parameters meniscus="5.85" dataStart="5.9" dataEnd="5.95" plateau="5.93" baseline="5.86"/>\n    </run>\n</experiment>\n',
        'Other_run.2401011200.RA.1.A.280.xml','c');

-- A time state for the first experiment.  It is a pair: the binary readings
-- in "data" and the field definitions that say what is in them, which
-- US_TimeState needs to make sense of the binary.  An export has to carry
-- both, so there is one here to export.
INSERT INTO timestate (timestateID, experimentID, filename, definitions, data)
VALUES (500, 10, 'MWL_demo_run.time_state.tmst',
        '<?xml version="1.0"?>\n<!DOCTYPE US_TimeState>\n<TimeState version="2.1" import_type="XLA">\n <file time_count="1" constant_incr="1" time_increment="1.0" first_time="0.0" ss_reso="100">\n  <value key="testkey" format="I4"/>\n </file>\n</TimeState>\n',
        'USTS2.1TMSTBYTES');
