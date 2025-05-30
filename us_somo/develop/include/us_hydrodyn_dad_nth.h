#ifndef US_HYDRODYN_DAD_NTH_H
#define US_HYDRODYN_DAD_NTH_H

#include "us_hydrodyn_dad.h"
#include "qlabel.h"
#include "qstring.h"
#include "qlayout.h"
#include "qvalidator.h"
//Added by qt3to4:
#include <QCloseEvent>

using namespace std;

class US_EXTERN US_Hydrodyn_Dad_Nth : public QDialog
{
   Q_OBJECT

   public:
      US_Hydrodyn_Dad_Nth(
                                void                     *              us_hydrodyn_dad,
                                map < QString, QString > *              parameters,
                                QWidget *                               p = 0,
                                const char *                            name = 0
                                );

      ~US_Hydrodyn_Dad_Nth();

   private:

      US_Config *                             USglobal;

      QLabel *                                lbl_title;


      QLabel *                                lbl_files;
      QListWidget *                              lb_files;

      QLabel *                                lbl_files_sel;
      QListWidget *                              lb_files_sel;
      QLabel *                                lbl_files_selected;

      QLabel *                                lbl_select_nth;

      QLabel *                                lbl_n;
      QLineEdit *                             le_n;

      QLabel *                                lbl_start;
      QLineEdit *                             le_start;
      QLabel *                                lbl_start_name;

      QLabel *                                lbl_end;
      QLineEdit *                             le_end;
      QLabel *                                lbl_end_name;

      QPushButton *                           pb_nth_only;
      QPushButton *                           pb_nth_add;

      QLabel *                                lbl_contain;

   // QLabel *                                lbl_starts_with;
   // QLineEdit *                             le_starts_with;

      QLabel *                                lbl_contains;
      QLineEdit *                             le_contains;

   // QLabel *                                lbl_ends_with;
   // QLineEdit *                             le_ends_with;

      QPushButton *                           pb_contains_only;
      QPushButton *                           pb_contains_add;

      QLabel *                                lbl_pvalue;

      QCheckBox     *                         cb_pvalues_green;
      QCheckBox     *                         cb_pvalues_yellow;
      QCheckBox     *                         cb_pvalues_red;

      map < QString, double >  *              ggaussian_last_pfit_map;
      double                                  alpha;
      double                                  alpha_over_5;

      QPushButton *                           pb_pvalues_only;
      QPushButton *                           pb_pvalues_add;

      QPushButton *                           pb_help;
      QPushButton *                           pb_quit;
      QPushButton *                           pb_go;

      void                     *              us_hydrodyn_dad;
      map < QString, QString > *              parameters;

      void                                    setupGUI();

   private slots:

      void                                    nth_only();
      void                                    nth_add();

      void                                    contains_only();
      void                                    contains_add();

      void                                    pvalues_only();
      void                                    pvalues_add();

      void                                    update_files_selected();

      void                                    go();
      void                                    quit();
      void                                    help();

      void                                    update_enables();

   protected slots:

      void                                    closeEvent( QCloseEvent * );
};

#endif
