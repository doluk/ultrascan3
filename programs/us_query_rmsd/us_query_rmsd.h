#ifndef US_QUERY_RMSD_H
#define US_QUERY_RMSD_H

#include "us_widgets.h"
#include "us_passwd.h"
#include "us_db2.h"
#include "us_model.h"
#include "us_dataIO.h"
#include "us_noise.h"
#include "../us_fematch/us_fematch.h"

#ifndef DbgLv
#define DbgLv(a) if(dbg_level>=a)qDebug()
#endif

class US_QueryRmsd : public US_Widgets{
   Q_OBJECT

   public:
   US_QueryRmsd();

   private:

   struct DataBundle
   {
      QString cell;
      QString channel;
      QString lamda;
      QString edit;
      QString analysis;
      QString method;
      QString editFile;
      QString rdataFile;
      int editID;
      int rdataID;
      int modelID;
      int expID;
      double rmsd;
   };

   QPushButton *pb_simulate;
   int dbg_level;
   double threshold;
   QTableWidget *tw_rmsd;
   QHeaderView *hheader;
   US_Passwd pw;
   US_DB2* dbCon;
   QVector<DataBundle> allData;
   QVector<int> selIndex;
   QMap<int, US_Model > Models;  //DB model id -> Model
   QMap<int, US_DataIO::EditedData> editData;  //DB edit id -> EditedData
   QMap<int, US_DataIO::RawData> rawData;      //DB RawData id -> RawData

   QStringList methodList;
   QStringList editList;
   QStringList analysisList;
   QStringList channelList;
   QStringList cellList;
   QStringList lambdaList;

   QLineEdit *le_runid;
   QLineEdit *le_file;
   QComboBox *cb_edit;
   QComboBox *cb_analysis;
   QComboBox *cb_cell;
   QComboBox *cb_channel;
   QComboBox *cb_lambda;
   QComboBox *cb_method;
   QLineEdit *le_threshold;

   QProgressBar *progress;

   US_FeMatch *fematch;

   bool check_connection(void);
   void clear_data(void);
   bool check_combo_content(QComboBox*, QString&);
   void highlight(void);
   bool get_metadata(DataBundle&, QString&);
   bool load_data(int, QString&);

   protected:
   void closeEvent(QCloseEvent*) override;

   private slots:
   void load_runid(void);
   void fill_table(int);
   void set_analysis(int);
   void set_method(int);
   void set_triple(int);
   void save_data(void);
   void simulate(void);
   void new_threshold(void);
   void update_progress(int);
};

class DoubleTableWidgetItem : public QTableWidgetItem
{
public:
   DoubleTableWidgetItem(double value) : QTableWidgetItem(QString::number(value, 'f', 8)), m_value(value) {}

   bool operator<(const QTableWidgetItem &other) const override
   {
      return m_value < other.data(Qt::EditRole).toDouble();
   }

   double get_value()
   {
      return m_value;
   }


private:
   double m_value;

};
#endif
