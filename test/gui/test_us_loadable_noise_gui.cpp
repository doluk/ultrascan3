// Holds the import tests' copy of the analysis programs' noise rule to the
// real one: US_LoadableNoise::count_noise() picks the latest model with
// noise of an edit, whose noise it loads by default, and
// DataPubNoiseRules::latestModelOnDisk() (test/programs/us_data_publication)
// is what the data publication tests compare a target with its source by.

#include "us3_gui_test_main.h"

#include "datapub_noise_rules.h"
#include "us_dataIO.h"
#include "us_loadable_noise.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_settings.h"
#include "us_util.h"

class US_LoadableNoiseTest : public QObject
{
    Q_OBJECT

private:
    QString editGUID;

    static QString dataDir()
    {
        return US_Settings::dataDir();
    }

    void clearStore()
    {
        QDir(dataDir() + "/models").removeRecursively();
        QDir(dataDir() + "/noises").removeRecursively();
        QDir().mkpath(dataDir() + "/models");
        QDir().mkpath(dataDir() + "/noises");
    }

    // A model of the edit in M<number>.xml
    QString model(int number, const QString& edit = QString(),
                  const QString& request = QString())
    {
        US_Model model;
        model.modelGUID   = US_Util::new_guid();
        model.editGUID    = edit.isEmpty() ? editGUID : edit;
        model.description = QString("run.1A280.e1_a%1_2DSA.model").arg(number);

        if (!request.isEmpty())
        {
            model.monteCarlo  = true;
            model.requestGUID = request;
        }

        const QString path = QString("%1/models/M%2.xml").arg(dataDir())
                             .arg(number, 7, 10, QChar('0'));
        if (model.write(path) != IUS_DB2::OK)
            qWarning() << "cannot write" << path;
        return model.modelGUID;
    }

    // Two noise records (ti, ri) of a model in N<number>.xml and the next
    void noise(int number, const QString& modelGUID)
    {
        for (int kk = 0; kk < 2; kk++)
        {
            US_Noise noise;
            noise.noiseGUID   = US_Util::new_guid();
            noise.modelGUID   = modelGUID;
            noise.type        = kk == 0 ? US_Noise::TI : US_Noise::RI;
            noise.description = "run.1A280.noise";
            noise.minradius   = 5.8;
            noise.maxradius   = 5.95;
            for (int ii = 0; ii < 8; ii++)
                noise.values << 0.001 * ii;
            noise.count = noise.values.size();
            noise.write(QString("%1/noises/N%2.xml").arg(dataDir())
                        .arg(number + kk, 7, 10, QChar('0')));
        }
    }

    // The model count_noise() puts first: the one whose noise it loads
    QString countNoiseLatest()
    {
        US_DataIO::EditedData edata;
        edata.editGUID = editGUID;

        US_LoadableNoise loader;
        QStringList      models;
        QStringList      noises;
        const int        count = loader.count_noise(true, &edata, nullptr,
                                                    models, noises);
        return (count > 0 && !models.isEmpty()) ? models.first() : QString();
    }

private slots:
    void init()
    {
        editGUID = US_Util::new_guid();
        US_Settings::set_noise_dialog(0);      // no question asked
        clearStore();
    }

    // The last file of a model with noise, whatever the order they were
    // made in, and not a later model without noise or of another edit
    void theLastModelFileWithNoiseIsTheLatest()
    {
        const QString first  = model(1);
        const QString second = model(2);
        model(3);                                     // no noise
        const QString other  = model(4, US_Util::new_guid());
        noise(1, second);
        noise(3, first);
        noise(5, other);

        QCOMPARE(countNoiseLatest(), second);
        QCOMPARE(DataPubNoiseRules::latestModelOnDisk(dataDir(), editGUID),
                 second);
    }

    // A model written into a gap of the file numbers counts as older than
    // the files after the gap, however new it is
    void aModelInAGapIsOlder()
    {
        const QString late  = model(5);
        const QString early = model(9);
        const QString gap   = model(2);
        noise(1, late);
        noise(3, early);
        noise(5, gap);

        QCOMPARE(countNoiseLatest(), early);
        QCOMPARE(DataPubNoiseRules::latestModelOnDisk(dataDir(), editGUID),
                 early);
    }

    // The models of one Monte Carlo request count once, by the first file
    void aMonteCarloRequestCountsOnce()
    {
        const QString request = US_Util::new_guid();
        const QString plain   = model(1);
        const QString mc1     = model(2, QString(), request);
        const QString mc2     = model(3, QString(), request);
        noise(1, plain);
        noise(3, mc1);
        noise(5, mc2);

        QCOMPARE(countNoiseLatest(), mc1);
        QCOMPARE(DataPubNoiseRules::latestModelOnDisk(dataDir(), editGUID),
                 mc1);
    }

    void noModelWithNoiseIsNone()
    {
        model(1);

        QCOMPARE(countNoiseLatest(), QString());
        QCOMPARE(DataPubNoiseRules::latestModelOnDisk(dataDir(), editGUID),
                 QString());
    }
};

US3_GUI_TEST_MAIN(US_LoadableNoiseTest)

#include "test_us_loadable_noise_gui.moc"
