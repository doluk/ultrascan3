//! \file us_stats_engine.h
#ifndef US_STATS_ENGINE_H
#define US_STATS_ENGINE_H

#include <QString>
#include <QVector>
#include "us_extern.h"



//! \brief Statistical analysis engine for spatio-temporal residuals
class US_UTIL_EXTERN US_StatsEngine
{
   public:
      //! \brief Global scalar metrics ("Health Check")
      struct US_UTIL_EXTERN GlobalMetrics
      {
         GlobalMetrics();

         double excessKurtosis;     //!< Excess kurtosis
         double ksStatistic;        //!< Kolmogorov-Smirnov statistic
         double hMetric;            //!< H metric (normalized sum of square differences from normal)
         double durbin_watson;      //!< Durbin-Watson
         bool   valid;              //!< Whether metrics are valid
         QString errorMessage;      //!< Error message if invalid
      };

      //! \brief Temporal diagnostics ("Drift Check")
      struct US_UTIL_EXTERN TemporalMetrics
      {
         QVector< double > meanPerScan;      //!< Mean residual per scan (time series)
         QVector< double > rmsdPerScan;      //!< RMSD per scan (time series)
         QVector< double > stddevPerScan;    //!< Stddev per scan (time series)
         QVector< double > fftMagnitude;     //!< FFT magnitude spectrum (DC removed)
         QVector< double > fftMagnitudeRaw;  //!< FFT magnitude spectrum (with DC)
         QVector< double > fftFrequency;     //!< FFT frequency bins
         double durbin_watson;                //!< Durbin-Watson on mean time series
         double globalRMSD;                  //!< Global RMSD across all data
         bool   valid;                       //!< Whether metrics are valid
      };

      //! \brief Spatial diagnostics ("Sensor Check")
      struct US_UTIL_EXTERN SpatialMetrics
      {
         QVector< double > meanPerPosition;  //!< Mean residual per spatial position
         QVector< double > stddevPerPosition;//!< Stddev per spatial position
         QVector< double > rmsdPerPosition;  //!< RMSD per spatial position
         double spatialVariance;             //!< Variance of spatial means
         bool   valid;                       //!< Whether metrics are valid
      };

      //! \brief SVD mode (principal component)
      struct US_UTIL_EXTERN SVDMode
      {
         QVector< double > spatial_vector;   //!< Spatial pattern (normalized shape)
         QVector< double > temporal_vector;  //!< Temporal trend (scaled by singular value)
         double singular_value;              //!< Magnitude (variance explained)
         double variance_fraction;           //!< Fraction of total variance
      };

      //! \brief SVD decomposition results
      struct US_UTIL_EXTERN SVDResults
      {
         QVector< SVDMode > modes;           //!< First N modes
         QVector< double > all_singular_values; //!< All singular values (for scree plot)
         bool valid;                         //!< Whether decomposition succeeded
         QString errorMessage;               //!< Error message if invalid
      };

      //! \brief DMD mode (dynamic mode with temporal evolution)
      struct US_UTIL_EXTERN DMDMode
      {
         QVector< double > spatial_pattern;  //!< Spatial pattern (real part of mode)
         QVector< double > spatial_pattern_imag; //!< Spatial pattern (imaginary part)
         QVector< double > temporal_evolution; //!< Reconstructed temporal evolution
         double growth_rate;                 //!< Growth/decay rate (real part of eigenvalue)
         double frequency;                   //!< Oscillation frequency (imaginary part of eigenvalue)
         double amplitude;                   //!< Mode amplitude
         double period;                      //!< Oscillation period (scans), NaN if non-oscillatory
         QString description;                //!< Human-readable description of mode behavior
         QVector<QVector<double>> reconstruction;
      };

      //! \brief DMD decomposition results
      struct US_UTIL_EXTERN DMDResults
      {
         QVector< DMDMode > modes;           //!< Extracted DMD modes
         QVector< QVector< double > > reconstruction; //!< Reconstructed residual matrix
         double reconstruction_error;        //!< Frobenius norm of reconstruction error
         bool valid;                         //!< Whether decomposition succeeded
         QString errorMessage;               //!< Error message if invalid
      };

      //! \brief Calculate global scalar metrics from flattened data
      //! \param data_flat  Flattened data vector
      static GlobalMetrics calculateGlobalMetrics( const QVector< double >& data_flat );

      //! \brief Calculate temporal diagnostics from spatio-temporal matrix
      //! \param residual_matrix  2D matrix [scans][positions]
      static TemporalMetrics calculateTemporalMetrics( const QVector< QVector< double > >& residual_matrix );

      //! \brief Calculate spatial diagnostics from spatio-temporal matrix
      //! \param residual_matrix  2D matrix [scans][positions]
      static SpatialMetrics calculateSpatialMetrics( const QVector< QVector< double > >& residual_matrix );

      //! \brief Perform SVD decomposition and extract principal modes
      //! \param residual_matrix  2D matrix [scans][positions]
      //! \param n_modes        Number of modes to extract (default 3)
      static SVDResults calculateSVD( const QVector< QVector< double > >& residual_matrix, int n_modes = 3 );

      //! \brief Perform Dynamic Mode Decomposition on spatio-temporal residuals
      //! \param residual_matrix  2D matrix [scans][positions]
      //! \param n_modes        Number of modes to extract (default 5)
      //! \param rank_truncation SVD rank for low-rank approximation (-1 = auto, 0 = full rank)
      //! DMD identifies coherent spatio-temporal structures and their dynamics:
      //!   - Growth/decay rates (stability of patterns)
      //!   - Oscillation frequencies (periodic patterns)
      //!   - Spatial patterns (where errors occur)
      static DMDResults calculateDMD( const QVector< QVector< double > >& residual_matrix,
                                      int n_modes = 5,
                                      int rank_truncation = -1 );

      //! \brief Filter out non-finite values from data vector
      //! \param data  Input data vector
      static QVector< double > filterFiniteValues( const QVector< double >& data );

      //! \brief Flatten a 2D matrix [scans][positions] of residuals into a 1D vector
      static QVector< double > flatten_residuals( const QVector< QVector< double > >& residual_matrix );

      // Global metrics helpers
      //! \brief Calculate excess kurtosis of data
      static double calculateExcessKurtosis( const QVector< double >& data, double mean, double stddev );
      //! \brief Calculate Kolmogorov-Smirnov statistic for data
      //! \param data    QVector<double> flattened residuals
      //! \param mean    double Mean of the residuals
      //! \param stddev  double Standard deviation of the residuals
      static double calculateKSStatistic( const QVector< double >& data, double mean, double stddev );
      static double calculateHMetric( const QVector< double >& data, double rmsd );

      // Temporal helpers
      static double calculateDurbinWatson( const QVector< double >& data );
      static void calculateFFT(
         const QVector< double >& timeSeries,
         QVector< double >& magnitude,
         QVector< double >& frequency,
         bool removeDC = true );
      static void calculateFFTRaw(
         const QVector< double >& timeSeries,
         QVector< double >& magnitude,
         QVector< double >& frequency );

      // Utility functions
      static double normalCDF( double x );
};

#endif // US_STATS_ENGINE_H
