#ifndef US_BUOYANCY_H
#define US_BUOYANCY_H

#include "us_extern.h"
#include "us_widgets.h"
#include "us_help.h"
#include "us_plot.h"
#include "us_dataIO.h"
#include "us_simparms.h"
#include "us_editor.h"
#include "us_minimize.h"
#include "us_extinctfitter_gui.h"
#include "qwt_plot_marker.h"

struct DataPoint
{
  QString name, description, dataset, triple;
  double peakPosition, peakDensity, peakVbar, peakGaussArea, percentTotal, percentTotal_uncorrected, temperature, bufferDensity, centerpiece;
  double meniscus, bottom, speed, gradientMW, gradientC0, gradientVbar, stretch, sigma;
};

struct cellInfo
{
  QString     cellName;
  QString     channelName;
  int         centerpieceID;
};


class US_Buoyancy : public US_Widgets
{
  Q_OBJECT

  public:
      US_Buoyancy();
      US_Buoyancy(QString auto_mode);
  
      bool us_buoyancy_auto_mode;

  signals:
      void widgetClosed();

  protected:
      void closeEvent(QCloseEvent *event) override;

  private:

      QVector <WavelengthScan> v_wavelength;
      US_ExtinctFitter *fitter;
      double * fitparameters;
            
  QMap< QString, QVector<double> > xfit_data;
  QMap< QString, QVector<double> > yfit_data;
  QMap< QString, QVector<double> > triple_name_to_Cdata;
  
      QMap < QString, QMap < double, QMap < int, QVector< double > > > > xfit_data_all_orders;
      QMap < QString, QMap < double, QMap < int, QVector< double > > > > yfit_data_all_orders;
      QMap < QString, QMap < double, QMap < int, double > > > variance_triple_order_map;
  
      // QMap< QString, QMap< int, QVector<double> > > xfit_data_all_orders;
      // QMap< QString, QMap< int, QVector<double> > > yfit_data_all_orders;
      // QMap< QString, QMap< int, double >> variance_triple_order_map;
      int current_order;
      double current_sigma;

      US_DataIO::RawData               data;
      QList   < US_DataIO::SpeedData > sData;
      QVector < US_DataIO::RawData >   allData;
      QVector < double >               meniscus;
      QVector < DataPoint >            dpoint;

  
      QMap< QString, QVector<double> > triple_name_to_peaks_map;
  QMap< QString, double > triple_name_to_total_area;
  QMap< QString, double > triple_name_to_total_area_uncorrected;
  QMap< QString, double > triple_name_to_rmsd;
  QMap< QString, double* > triple_name_to_fit_parameters;
      QMap< QString, QVector< QwtPlotCurve* > > triple_name_to_peak_curves_map;
  QMap< QString, QVector< QwtPlotCurve* > > triple_name_to_peak_gauss_envelopes_map;
      QMap< QString, QMap < QString, QStringList > >  triple_name_to_peak_to_parms_map;
      QMap< QString, bool > triple_report_saved_map;
      QMap< QString, bool > triple_fitted_map;
      QMap< QString, bool > triple_peaks_defined_map;
  QMap< QString, bool > do_fit_for_triple;

  QMap< QString, double > alpha_centerpiece;
  QMap< QString, double > data_left_to_triple_name_map;
  QMap< QString, double > data_right_to_triple_name_map;
  QMap< QString, double > meniscus_to_triple_name_map;
  QMap< QString, double > buffDensity_to_triple_name_map;
  QMap< QString, double > sigma_to_triple_name_map;
  QMap< QString, double > gradMW_to_triple_name_map;
  QMap< QString, double > gradVbar_to_triple_name_map;
  QMap< QString, double > gradC0_to_triple_name_map;

  QMap< QString, int >    gauss_order_minVariance;
  QMap< QString, double > sigma_val_minVariance;
  
      QRadioButton*      rb_meniscus;
      QRadioButton*      rb_datapoint;
      DataPoint          tmp_dpoint;

      int                current_triple;
      int                current_scan;
      int                total_speeds;
      int                dbg_level;

      bool               expIsBuoyancy;

      double             bottom;
      double             bottom_calc;
      double             current_rpm;
      double             current_stretch;
      double             current_point; // contains x coordinate picked by the user

      QList< int >       sd_offs;    // speed data offsets, ea. triple
      QList< int >       sd_knts;    // speed data counts, ea. triple

      US_Help            showHelp;
      US_Editor*         te;
      
      QVector <US_SimulationParameters> simparams;

      QString            workingDir;
      QString            runID;
      QString            editID;
      QString            dataType;
      QString            expType;
      QStringList        files;
      QStringList        triples;
      QStringList        trip_rpms;

      QPushButton*       pb_details;
      QPushButton*       pb_load;

      QwtPlot*           data_plot;
      QwtPlotCurve*      v_line;
      QwtPlotCurve*      minimum_curve;
      QwtPlotGrid*       grid;
      QwtPlotMarker*     marker;
      US_PlotPicker*     pick;
      US_Plot*           plot;

      QLabel*            lbl_rpms;
      QLabel*            lbl_stretch;
      QLabel*            lbl_dens_0;
      QLabel*            lbl_bottom;
      QLabel*            lbl_bottom_calc;
      QLabel*            lbl_vbar;
      QLabel*            lbl_MW;
      QLabel*            lbl_meniscus;
      QLabel*            lbl_temperature;
      QLabel*            lbl_peakVbar;
      QLabel*            lbl_peakPosition;
      QLabel*            lbl_peakDensity;
      QLabel*            lbl_peakName;
      QLabel*            lbl_buffer_density;

      QLineEdit*         le_info;
      QLineEdit*         le_stretch;
      QLineEdit*         le_investigator;
      QLineEdit*         le_dens_0;
      QLineEdit*         le_bottom;
      QLineEdit*         le_bottom_calc;
      QLineEdit*         le_vbar;
      QLineEdit*         le_MW;
      QLineEdit*         le_meniscus;
      QLineEdit*         le_sigma;
      QLineEdit*         le_temperature;
      QLineEdit*         le_peakVbar;
      QLineEdit*         le_peakPosition;
      QLineEdit*         le_peakDensity;
      QLineEdit*         le_peakName;
      QLineEdit*         le_peakGaussArea;
  
      QLineEdit*         le_buffer_density;

      US_Disk_DB_Controls* disk_controls; //!< Radiobuttons for disk/db choice

      QComboBox*         cb_triple;
      QComboBox*         cb_rpms;

      QComboBox*         cb_peaks;

      QPushButton*       pb_write;
      QPushButton*       pb_save;

      QPushButton*       pb_delete_peak;
      QPushButton*       pb_add_peak;

  QPushButton*       pb_fit_current_triple;
  QCheckBox*         ck_auto_fit;
  
      QProgressBar*      pgb_progress;
      QPushButton*       pb_view_reports;

      QwtCounter*        ct_selectScan;

private slots:
	double calc_stretch       ( void );
  void draw_vline           ( double );
  void draw_vline_auto      ( double, QString );
  void draw_gauss_envelope  (  QMap < QString, QStringList > ); 
	void mouse                ( const QwtDoublePoint& );
  void mouse_peak                ( const QwtDoublePoint& );
  void sel_investigator     ( void );
	void update_disk_db       ( bool );
	void load                 ( void );
	void details              ( void );
	void new_triple           ( int  );
        void new_peak             ( int );
	void plot_scan            ( double );
  
  void update_for_sigma  (void);
  void update_for_MW     (void);
  void update_for_vbar   (void);
  void update_for_dens_0 (void);

  void write                ( void );
        void write_auto           ( void );
        void save                 ( void );
        void save_auto            ( QString );
	void reset                ( void );
        void calc_points          ( void );
        void calc_points_auto     ( QString );
  QMap< QString, double > find_closest_sigma_height( QString, double );
  QVector< double > identify_peaks ( QString, double );
        int index_of_data( QVector<double>, double );
       bool isMaximum_y( QVector<double>, int, int, int, QString );
  QMap< QString, double > get_data_conf_from_edit_profile ( QString, QString );
       
        void process_yfit( QVector <QVector<double> > &x, QVector <QVector<double> > &y );
        void process_variance( double );
        double compute_rmsd( QString ); 
        void delete_peak( void );
        void add_peak( void );
  QMap<QString, double> calc_gauss_area( QString, double, double, double );
  QMap<QString, double> calc_total_area( QString );

  void enblFitBtn( bool );
  void fit_current_triple( void );
  
  
  void print_xy( US_DataIO::RawData, int  );
	void new_rpmval           ( int  );
	void update_fields        ( void );
        void update_speedData     ( void );
	void update_dens_0        ( void );
        void update_bufferDensity ( void );
	void update_bottom        ( void );
	void update_bottom_calc   ( void );
	void update_vbar          ( void );
	void update_MW            ( void );
        void update_peakName      ( void );
	void update_meniscus      ( void );
	void help                 ( void )
        { showHelp.show_help( "manual/us_buoyancy.html" ); };
};
#endif
