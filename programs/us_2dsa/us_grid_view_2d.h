//! \file us_grid_view_2d.h
#ifndef US_GRID_VIEW_2D_H
#define US_GRID_VIEW_2D_H

#include "us_extern.h"
#include "us_widgets_dialog.h"
#include "us_plot.h"
#include "us_help.h"
#include "us_solute.h"
#include "us_2dsa_process.h"
#include "qwt_plot_curve.h"
#include "qwt_symbol.h"
#include "qwt_plot_picker.h"

//! \brief A class to provide a window displaying the 2DSA solute grid
//!        and its subgrids, similar to the Custom Grid Editor display.
//!        When fit task records are given, the grid of each refinement
//!        iteration is shown along with the solutes fitted by each
//!        subgrid (and deeper) task and by the final fit.
class US_GridView2D : public US_WidgetsDialog
{
   Q_OBJECT

   public:
      //! \brief Attribute types that may be plotted on the axes
      enum AttrType { ATTR_S, ATTR_K, ATTR_M, ATTR_V, ATTR_D };

      //! \brief US_GridView2D constructor
      //! \param subgrids The grid solutes as a list of subgrids
      //! \param desc     Description of the grid (e.g., "Uniform Grid")
      //! \param p        Pointer to the parent of this widget
      US_GridView2D( const QList< QVector< US_Solute > >&,
                     const QString&, QWidget* p = 0 );

      //! \brief Replace the grid displayed when fit results are not shown
      //! \param subgrids The grid solutes as a list of subgrids
      //! \param desc     Description of the grid
      void set_grid( const QList< QVector< US_Solute > >&, const QString& );

      //! \brief Replace the fit task records (empty list to clear)
      //! \param records  Task records from the 2DSA processor
      void set_fit_records( const QList< US_2dsaTaskRecord >& );

   private:
      //! \brief Point values for all attribute types
      struct GridPt
      {
         double vals[ 5 ];
      };

      QList< QVector< US_Solute > > sgrid_sols;  //!< Grid from settings
      QString                       sgrid_desc;  //!< Settings grid type
      QList< US_2dsaTaskRecord >    recs;        //!< Fit task records

      QList< QVector< US_Solute > > grid_sols;   //!< Displayed grid
      QList< QVector< GridPt > >    grid;        //!< Displayed grid points
      QList< int >                  stage_recs;  //!< Records of stage
      QVector< US_Solute >          added_sols;  //!< Refinement-added
      QVector< US_Solute >          final_sols;  //!< Final fit solutes
      QList< QwtPlotCurve* >        point_curves; //!< Grid curves
      QList< QwtPlotCurve* >        over_curves;  //!< Overlay curves
      QwtPlotCurve*                 hl_curve;     //!< Highlight curve
      QVector< US_Solute >          over_sols;    //!< Overlaid solutes
      QStringList                   over_kind;    //!< Overlaid solute kinds

      QwtPlot*         data_plot;
      QwtPlotPicker*   pick;

      QButtonGroup*    x_axis;
      QButtonGroup*    y_axis;

      QLabel*          lb_subgrid;
      QLabel*          lb_npoints_curr;

      QLineEdit*       le_desc;
      QLineEdit*       le_npoints;
      QLineEdit*       le_nsubgrids;
      QLineEdit*       le_npoints_curr;
      QLineEdit*       le_task_rmsd;
      QLineEdit*       le_iter_rmsd;
      QLineEdit*       le_nearest;

      QComboBox*       cb_stage;

      QwtCounter*      ct_subgrid;
      QwtCounter*      ct_iter;
      QwtCounter*      ct_size;

      QCheckBox*       ck_fitres;
      QCheckBox*       ck_highlight;
      QCheckBox*       ck_colorall;
      QCheckBox*       ck_tasksols;
      QCheckBox*       ck_allsols;
      QCheckBox*       ck_finalsols;
      QCheckBox*       ck_addsols;

      QColor           color_base;
      QColor           color_subgrid;
      QColor           color_tasksol;
      QColor           color_final;
      QColor           color_added;

      int              dbg_level;

      US_Help          showHelp;

      bool             show_fit     ( void );
      int              iter_count   ( void );
      int              final_rec    ( int );
      int              current_rec  ( void );
      void             build_grid   ( void );
      void             fill_stages  ( void );
      void             fill_tasks   ( void );
      void             plot_overlays( void );
      void             refresh_all  ( void );
      void             add_sol_curves( const QVector< US_Solute >&,
                                       QwtSymbol::Style, const QColor&,
                                       bool, bool, const QString&,
                                       const QString&, double );
      QwtPlotCurve*    point_curve  ( const QString&, bool );
      GridPt           make_point   ( const US_Solute& );
      QString          attr_title   ( int );
      QString          attr_symbol  ( int );
      QString          sol_text     ( const US_Solute& );
      QColor           subgrid_color( int );

   private slots:
      void plot_points     ( void );
      void plot_subgrid    ( void );
      void set_symbol_size ( double );
      void select_axis     ( int );
      void subgrid_changed ( double );
      void iter_changed    ( double );
      void stage_changed   ( int );
      void fitres_toggled  ( bool );
      void point_clicked   ( const QPointF& );
      void help            ( void )
      { showHelp.show_help( "manual/2dsa/2dsa_analys.html" ); };
};
#endif
