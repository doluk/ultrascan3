// US_SelectRuns with a run filter: the filter is in place for the first
// listing, and it is dropped when the user switches the source, since the
// caller chose the runs from the source the dialog opened with.

#include "us3_gui_test_main.h"

#include "us_select_runs.h"
#include "us_settings.h"

class US_SelectRunsGuiTest : public QObject
{
    Q_OBJECT

private:
    QStringList made;

    // A local run as the dialog lists it: a .auc file and the run's XML
    void makeRun(const QString& runID)
    {
        const QString dir = US_Settings::resultDir() + "/" + runID;
        QVERIFY(QDir().mkpath(dir));

        QFile auc(dir + "/" + runID + ".RA.1.A.280.auc");
        QVERIFY(auc.open(QIODevice::WriteOnly));
        auc.write("UCDA");
        auc.close();

        QFile xml(dir + "/" + runID + ".RA.xml");
        QVERIFY(xml.open(QIODevice::WriteOnly | QIODevice::Text));
        xml.write("<?xml version=\"1.0\"?><US_Scandata><experiment id=\"-1\">"
                  "<label>label</label></experiment></US_Scandata>");
        xml.close();

        made << dir;
    }

    static QStringList sorted(QStringList list)
    {
        list.sort();
        return list;
    }

private slots:
    void initTestCase()
    {
        makeRun("SelRuns_A");
        makeRun("SelRuns_B");
        makeRun("SelRuns_C");
    }

    void cleanupTestCase()
    {
        for (const QString& dir : made)
            QDir(dir).removeRecursively();
    }

    void theFilterLimitsTheFirstListing()
    {
        QStringList runIDs;
        US_SelectRuns dialog(false, runIDs,
                             QStringList() << "SelRuns_A" << "SelRuns_C");

        QVERIFY(dialog.isFiltered());
        QCOMPARE(sorted(dialog.offeredRuns()),
                 QStringList() << "SelRuns_A" << "SelRuns_C");
        QVERIFY(dialog.windowTitle().contains("filtered"));
    }

    void anEmptyFilterOffersEveryRun()
    {
        QStringList runIDs;
        US_SelectRuns dialog(false, runIDs, QStringList());

        QVERIFY(!dialog.isFiltered());
        QVERIFY(dialog.offeredRuns().contains("SelRuns_B"));
    }

    void switchingTheSourceDropsTheFilter()
    {
        QStringList runIDs;
        US_SelectRuns dialog(false, runIDs, QStringList() << "SelRuns_A");
        QCOMPARE(dialog.offeredRuns(), QStringList() << "SelRuns_A");

        // what the Disk/DB controls call; the local source again, so that
        // no database is needed
        QVERIFY(QMetaObject::invokeMethod(&dialog, "update_disk_db",
                                          Q_ARG(bool, false)));

        QVERIFY(!dialog.isFiltered());
        QVERIFY(!dialog.windowTitle().contains("filtered"));
        QVERIFY(dialog.offeredRuns().contains("SelRuns_B"));
        QVERIFY(dialog.offeredRuns().contains("SelRuns_C"));
    }
};

US3_GUI_TEST_MAIN(US_SelectRunsGuiTest)

#include "test_us_select_runs_gui.moc"
