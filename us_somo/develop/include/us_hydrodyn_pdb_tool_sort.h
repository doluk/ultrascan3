// ---------------------------------------------------------------------------------------------
// --------------- WARNING: this code is generated by an automatic code generator --------------
// ---------------------------------------------------------------------------------------------
// -------------- WARNING: any modifications made to this code will be overwritten -------------
// ---------------------------------------------------------------------------------------------

#ifndef US_HYDRODYN_PDB_TOOL_SORT_H
#define US_HYDRODYN_PDB_TOOL_SORT_H

#include "us_hydrodyn_pdb_tool.h"
#include "qlabel.h"
#include "qstring.h"
#include "qlayout.h"
#include "qlineedit.h"
#include "qfontmetrics.h"
#include "qcheckbox.h"
//Added by qt3to4:
#include <QCloseEvent>

using namespace std;

class US_EXTERN US_Hydrodyn_Pdb_Tool_Sort : public QDialog
{
   Q_OBJECT

   public:
      US_Hydrodyn_Pdb_Tool_Sort(
                                void                     *              us_hydrodyn,
                                map < QString, QString > *              parameters,
                                QWidget *                               p = 0,
                                const char *                            name = 0
                                );

      ~US_Hydrodyn_Pdb_Tool_Sort();

   private:

      US_Config *                             USglobal;

      QLabel *                                lbl_title;
      QLabel *                                lbl_credits_1;
      QLabel *                                lbl_residuesa;
      QLineEdit *                             le_residuesa;
      QLabel *                                lbl_residuesb;
      QLineEdit *                             le_residuesb;
      QLabel *                                lbl_reportcount;
      QLineEdit *                             le_reportcount;
      QCheckBox *                             cb_order;
      QCheckBox *                             cb_caonly;

      QPushButton *                           pb_help;
      QPushButton *                           pb_close;
      void                     *              us_hydrodyn;
      map < QString, QString > *              parameters;


      void                                    setupGUI();

   private slots:

      void                                    update_residuesa( const QString & );
      void                                    update_residuesb( const QString & );
      void                                    update_reportcount( const QString & );
      void                                    set_order();
      void                                    set_caonly();

      void                                    help();
      void                                    cancel();

   protected slots:

      void                                    closeEvent( QCloseEvent * );
};

#endif
