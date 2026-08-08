//! \file us_show_norm.h
#ifndef US_SHOW_NORM_H
#define US_SHOW_NORM_H

#include "us_extern.h"
#include "us_widgets_dialog.h"
#include "us_2dsa_process.h"
#include "us_plot.h"
#include "us_help.h"
#include "us_model.h"
#include "us_colorgradIO.h"
#include "us_spectrodata.h"
#include "qwt_plot_marker.h"
#include "qwt_plot_spectrogram.h"
#include "qwt_plot_layout.h"
#include "qwt_plot_zoomer.h"
#include "qwt_plot_panner.h"
#include "qwt_scale_widget.h"
#include "qwt_scale_draw.h"
#include "qwt_color_map.h"

//! \brief Description of the grid a set of A-column norms was computed on
//!
//! The norms themselves travel in a US_Model, one component per grid point,
//! with the norm carried in signal_concentration.  This structure carries
//! what the model cannot:  the shape of the grid, the NNLS cutoff that the
//! norms are compared against, and the coherence of each column with its
//! grid neighbours.
struct US_NormGridInfo
{
   US_NormGridInfo() : nss( 0 ), nks( 0 ), ndpts( 0 ), nsamp( 0 ),
                       sig_nrad( 0 ), sig_nscn( 0 ), norm_cut( 0.0 ) {}

   int     nss;                  //!< grid points per row (X direction)
   int     nks;                  //!< grid rows (Y direction)
   int     ndpts;                //!< data points behind each norm
                                 //!<  (scans x radial points)
   int     nsamp;                //!< values per column signature
   int     sig_nrad;             //!< radial points per signature
   int     sig_nscn;             //!< scans per signature
   double  norm_cut;             //!< NNLS column-norm cutoff in effect
   QString descr;                //!< run and grid description

   //! Cosine of the angle between each A column and its +X grid neighbour;
   //!  -1.0 where no neighbour exists or coherence was not computed
   QVector< double > coher_x;
   //! Cosine of the angle between each A column and its +Y grid neighbour
   QVector< double > coher_y;

   //! Unit-length subsampled signature of every A column, laid out as one
   //!  block of nsamp values per grid point, each block being sig_nscn
   //!  consecutive scans of sig_nrad readings.  The dot product of two
   //!  blocks is the cosine of the angle between those two columns, so any
   //!  pair can be compared, not only neighbours.
   QVector< float >  sigs;
   //! Norm of each subsampled column.  Multiplying a signature by this
   //!  recovers the simulated readings in OD.
   QVector< double > signorm;
   //! Radius of each sampled reading, sig_nrad values
   QVector< double > sig_radius;
   //! Elapsed run time of each sampled scan, sig_nscn values
   QVector< double > sig_time;
};

//! \brief Raster data that renders one flat cell per grid point
//!
//! The pseudo-3D raster this window used before spreads every point as a
//! Gaussian and merges overlapping points by maximum.  That suits a sparse
//! distribution from a fit, but a norm grid is dense and regular, and the
//! spreading blurs away exactly the point-to-point differences the window
//! exists to show.  This raster instead returns the value of the grid point
//! nearest the queried position, so each point owns a cell with a hard
//! edge, and no value is invented between grid points.  On a rectilinear
//! grid those cells are exactly the grid cells.
//!
//! Lookup uses a uniform bucket index over the plotted rectangle, searching
//! outward from the query position's bucket until no unexamined bucket can
//! hold anything closer.
class US_NormRasterData : public QwtRasterData
{
   public:
      US_NormRasterData();

      //! \brief Define the points, their Z range, and the plotted rectangle
      //! \param xpos  Plot X coordinate of each point
      //! \param ypos  Plot Y coordinate of each point
      //! \param zval  Z value of each point
      //! \param zmin  Z minimum for the color scale
      //! \param zmax  Z maximum for the color scale
      //! \param drect Plotted rectangle in plot coordinates
      void setPoints( const QVector< double >&, const QVector< double >&,
                      const QVector< double >&, double, double,
                      const QRectF& );

#if QWT_VERSION < QT_VERSION_CHECK(6, 3, 0)
      QwtInterval interval( Qt::Axis axis ) const;
#else
      QwtInterval interval( Qt::Axis axis ) const override;
#endif

      //! \brief Z value of the grid point nearest a plot position
      double value( double x, double y ) const override;

   private:
      QVector< double >       upos;      // normalized X of each point
      QVector< double >       vpos;      // normalized Y of each point
      QVector< double >       zpos;      // Z value of each point
      QVector< QVector< int > >  bkts;   // point indexes per bucket

      double  xmin;
      double  ymin;
      double  uscl;      // 1 / X range
      double  vscl;      // 1 / Y range
      double  xmax;
      double  ymax;
      double  zmin;
      double  zmax;
      double  cellm;     // smaller bucket dimension, normalized

      int     nbx;       // buckets across X
      int     nby;       // buckets across Y
      int     npts;      // number of points
};

//! \brief A window showing the A-matrix column norms of a 2DSA grid,
//!        together with the conditioning diagnostics that govern whether
//!        NNLS can separate neighbouring grid points.
class US_show_norm : public US_WidgetsDialog
{
   Q_OBJECT

   public:
      //! \param model      Model with one component per grid point, the
      //!                   column norm carried in signal_concentration
      //! \param cnst_vbar  Flag that vbar is constant over the grid (so
      //!                   that f/f0 is the varying Y attribute)
      //! \param ginfo      Grid shape, NNLS cutoff and coherence values
      //! \param p          Parent widget
      US_show_norm( US_Model*, bool, const US_NormGridInfo&, QWidget* = 0 );
      ~US_show_norm();

   private:
      // Note: held by value.  The caller passes a local flag, so a reference
      //  member would dangle once this modeless dialog outlives that frame.
      bool          cnst_vbar;
      US_Model*     model;

      enum attr_type { ATTR_S, ATTR_K, ATTR_W, ATTR_V, ATTR_D, ATTR_F };

      //! Quantity mapped onto the Z (color) axis
      enum z_mode
      {
         ZM_NORM,      //!< column norm, linear
         ZM_RMS,       //!< column norm per data point (RMS signal, OD)
         ZM_LOGNORM,   //!< column norm, log10
         ZM_RELX,      //!< norm as a percentage of the maximum at the same X
         ZM_DEVX,      //!< deviation from the mean over Y at the same X
         ZM_PCTTOT,    //!< norm as a percentage of the sum of all norms
         ZM_COH_PICK,  //!< collinearity with the selected grid point
         ZM_COH_X,     //!< collinearity with the +X grid neighbour
         ZM_COH_Y,     //!< collinearity with the +Y grid neighbour
         ZM_COH_MAX,   //!< worst collinearity with either grid neighbour
         ZM_COUNT      //!< number of Z modes
      };

      int           dbg_level;

      QWidget*      parentw;

      QLabel*       lb_plt_smin;
      QLabel*       lb_plt_smax;
      QLabel*       lb_plt_kmin;
      QLabel*       lb_plt_kmax;

      QTextEdit*    te_distr_info;

      QLineEdit*    le_cmap_name;
      QLineEdit*    le_pickinfo;
      QLineEdit*    le_compinfo;

      QComboBox*    cb_zmode;

      QwtCounter*   ct_plt_kmin;
      QwtCounter*   ct_plt_kmax;
      QwtCounter*   ct_plt_smin;
      QwtCounter*   ct_plt_smax;

      QwtPlot*      data_plot;
      QwtPlot*      xsec_plot_y;
      QwtPlot*      xsec_plot_x;
      QwtPlot*      scan_plot;
      QwtPlot*      diff_plot;

      US_PlotPicker* pick;
      US_PlotPicker* pick_sy;
      US_PlotPicker* pick_sx;

      QwtLinearColorMap*  colormap;

      QPushButton*  pb_ldcolor;
      QPushButton*  pb_close;
      QPushButton*  pb_reset;
      QPushButton*  pb_refresh;

      QwtCounter*   ct_nscans;

      QCheckBox*    ck_autosxy;
      QCheckBox*    ck_cutmark;
      QCheckBox*    ck_shapedf;

      QRadioButton* rb_x_s;
      QRadioButton* rb_x_ff0;
      QRadioButton* rb_x_mw;
      QRadioButton* rb_x_vbar;
      QRadioButton* rb_x_D;
      QRadioButton* rb_x_f;
      QRadioButton* rb_y_s;
      QRadioButton* rb_y_ff0;
      QRadioButton* rb_y_mw;
      QRadioButton* rb_y_vbar;
      QRadioButton* rb_y_D;
      QRadioButton* rb_y_f;
      QButtonGroup* bg_x_axis;
      QButtonGroup* bg_y_axis;

      US_NormGridInfo     ginfo;

      //! Plotted points:  s holds the X attribute, k the Y attribute and
      //!  c the Z value of the current Z mode
      QList< S_Solute >   xy_distro;

      //! Column norms, grid order, index = iy * nss + ix
      QVector< double >   gnorm;
      //! Unshifted Z values for the current Z mode, same order as gnorm
      QVector< double >   zvals;
      //! False where the current Z mode has no real value for a point (an
      //!  edge point with no neighbour).  The map must paint something
      //!  there, but the cross sections leave a gap rather than invent a
      //!  value and draw a cliff that is not in the data.
      QVector< bool >     zvalid;

      double        plt_smin;
      double        plt_smax;
      double        plt_kmin;
      double        plt_kmax;
      double        plt_zmin;
      double        plt_zmax;

      int           plot_x;
      int           plot_y;
      int           zmode;
      int           pick_ix;
      int           pick_iy;
      int           comp_ix;
      int           comp_iy;
      int           nscans;

      //! 1/sqrt(ndpts):  converts a column norm to the RMS signal per data
      //!  point, in OD, that one OD of that species would produce
      double        rms_scale;

      bool          auto_sxy;
      bool          have_grid;
      bool          have_coher;
      bool          have_sigs;
      bool          have_cut;
      bool          have_rms;
      bool          have_scans;
      bool          have_comp;

      QString       xa_title;
      QString       ya_title;
      QString       cmapname;

   private slots:
      void select_x_axis   ( int  );
      void select_y_axis   ( int  );
      void load_color      ( void );
      void update_plot_smin( double );
      void update_plot_smax( double );
      void update_plot_kmin( double );
      void update_plot_kmax( double );
      void set_limits      ( void );
      void reset           ( void );
      void plot_data       ( int  );
      void plot_data       ( void );
      void select_autosxy  ( void );
      void select_zmode    ( int  );
      void select_cutmark  ( void );
      void select_shapedf  ( void );
      void update_nscans   ( double );
      void pick_point      ( const QPointF& );
      void pick_sect_y     ( const QPointF& );
      void pick_sect_x     ( const QPointF& );

   private:
      //! \brief Rebuild the plotted point list from the current selections
      void   build_xy_distro( void );
      //! \brief Redraw the two cross-section plots
      void   plot_sections  ( void );
      //! \brief Redraw the simulated-scan and difference plots
      void   plot_scans     ( void );
      //! \brief Refresh the compared-point read-out
      void   report_comp    ( void );
      //! \brief Refresh the summary statistics box
      void   update_stats   ( void );
      //! \brief Value of one model attribute for one grid point
      double attr_value     ( int, int );
      //! \brief Short name of a plot attribute, for use in running text
      QString attr_label    ( int  );
      //! \brief Check and enable the axis radios to match plot_x, plot_y
      void   sync_axis_buttons( void );
      //! \brief Apply the limit labels and counter ranges of the current axes
      void   apply_axis_ranges( void );
      //! \brief Fill zvals from gnorm and the coherences, per the Z mode
      void   compute_zvals  ( void );
      //! \brief Cosine of the angle between two A columns, from signatures
      double coher_pair     ( int, int );
      //! \brief Grid index of the point nearest a plot position, or -1
      int    nearest_point  ( const QPointF& );
      //! \brief Number of grid points whose norm falls below the NNLS cutoff
      int    count_cut      ( void );
      //! \brief Title of the Z axis, per the current Z mode
      QString zmode_title   ( void );
      //! \brief True when the current Z mode is a collinearity map
      bool   zmode_is_coher ( void );
      //! \brief Annotation title for a plot attribute index
      QString anno_title    ( int  );
      //! \brief Make a copy of a color map and return a pointer to it
      QwtLinearColorMap* ColorMapCopy( QwtLinearColorMap* );
};
#endif

