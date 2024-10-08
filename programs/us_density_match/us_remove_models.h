//! \file us_remove_distros.h
#ifndef US_RMVMODEL_H
#define US_RMVMODEL_H

#include "us_extern.h"
#include "us_widgets_dialog.h"
#include "us_density_match.h"
#include "us_help.h"

#include <qwt_plot.h>

//! \brief A class to provide a window for remove_distros analysis controls

class US_RemoveModels : public US_WidgetsDialog
{
   Q_OBJECT

   public:
      //! \brief US_RemoveModels constructor
      //! \param adistros Pointer to model distributions list
      //! \param p        Pointer to the parent of this widget
      US_RemoveModels( QVector< DisSys >&, QWidget* p = 0 );

   private:
      QVector< DisSys >&  distros; // Reference to model distributions vector

      int           nd_orig;       // Number of distributions in original
      int           nd_removed;    // Number of total distributions removed
      int           nd_selected;   // Number of currently selected distributions

      QGridLayout*  mainLayout;    // 

      QStringList   mdesc_orig;    // List of descriptions from original
      QStringList   mdesc_list;    // List of descriptions now listed

      QListWidget*  lw_distrs;     // List widget of distributions

      QTextEdit*    te_status;     // Status text box

      QPushButton*  pb_restore;    // Restore button
      QPushButton*  pb_remove;     // Remove  button
      QPushButton*  pb_help;       // Help    button
      QPushButton*  pb_cancel;     // Cancel  button
      QPushButton*  pb_accept;     // Accept  button

   protected:
      US_Help       showHelp;

   private slots:

      void selectionsChanged( void );
      void remove           ( void );
      void restore          ( void );
      void accepted         ( void );
      void help             ( void )
      { showHelp.show_help( "dens_match_rmvmods.html" ); };
};
#endif

