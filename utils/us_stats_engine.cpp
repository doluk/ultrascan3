//! \file us_stats_engine.cpp

#include "us_stats_engine.h"
//#include <Eigen/Dense>
//#include <Eigen/SVD>
#include <algorithm>
#include <cmath>
#include <qmath.h>


// Utility functions
QVector<double> US_StatsEngine::filterFiniteValues( const QVector<double>& data )
{
   QVector<double> filtered;
   filtered.reserve( data.size() );

   for ( double val : data )
   {
      if ( std::isfinite( val ) )
      {
         filtered.append( val );
      }
   }

   return filtered;
}

QVector<double> US_StatsEngine::flatten_residuals( const QVector<QVector<double>>& residual_matrix )
{
   QVector<double> flat;
   const int       scanCount = residual_matrix.size();
   const int       valCount  = residual_matrix[0].size();

   flat.reserve( scanCount * valCount );

   for ( int ii = 0; ii < scanCount; ii++ )
   {
      for ( int jj = 0; jj < valCount; jj++ )
      {
         flat.append( residual_matrix[ii][jj] );
      }
   }

   return flat;
}

double US_StatsEngine::normalCDF( const double x )
{
   // Approximation of the cumulative distribution function for standard normal
   // Using error function: CDF(x) = 0.5 * (1 + erf(x / sqrt(2)))
   return 0.5 * ( 1.0 + std::erf( x / std::sqrt( 2.0 ) ) );
}

double US_StatsEngine::calculateExcessKurtosis( const QVector<double>& data, const double mean, const double stddev )
{
   if ( stddev <= 0.0 || data.size() < 4 )
   {
      return 0.0;
   }

   double    mu4 = 0.0;
   const int n   = data.size();

   for ( const double val : data )
   {
      const double diff = val - mean;
      mu4 += std::pow( diff, 4.0 );
   }

   mu4 /= n;
   const double sigma4 = std::pow( stddev, 4.0 );

   return ( mu4 / sigma4 ) - 3.0;
}


double US_StatsEngine::calculateKSStatistic( const QVector<double>& data, const double mean, const double stddev )
{
   if ( stddev <= 0.0 || data.size() < 2 )
   {
      return 0.0;
   }

   // Sort data
   QVector<double> sorted_data = data;
   std::sort( sorted_data.begin(), sorted_data.end() );

   const int n        = sorted_data.size();
   double    max_diff = 0.0;

   for ( int i = 0; i < n; i++ )
   {
      // Empirical CDF at this point
      const double e_cdf = static_cast<double>( i + 1 ) / n;

      // Theoretical normal CDF (standardized)
      const double z     = ( sorted_data[i] - mean ) / stddev;
      const double t_cdf = normalCDF( z );

      const double diff = std::abs( e_cdf - t_cdf );
      max_diff          = std::max( max_diff, diff );
   }

   return max_diff;
}

double US_StatsEngine::calculateDurbinWatson( const QVector<double>& data )
{
   if ( data.size() < 2 )
   {
      return 2.0;
   }

   double numerator   = 0.0;
   double denominator = 0.0;

   for ( int i = 1; i < data.size(); i++ )
   {
      const double diff = data[i] - data[i - 1];
      numerator += diff * diff;
   }

   for ( const double i : data )
   {
      denominator += i * i;
   }

   if ( denominator <= 0.0 )
   {
      return 2.0;
   }

   return numerator / denominator;
}

void US_StatsEngine::calculateFFT( const QVector<double>& timeSeries, QVector<double>& magnitude,
                                   QVector<double>& frequency, const bool removeDC )
{
   const int N = timeSeries.size();
   if ( N < 4 )
   {
      magnitude.clear();
      frequency.clear();
      return;
   }

   QVector<double> data_to_transform = timeSeries;

   if ( removeDC )
   {
      // CRITICAL: Remove DC offset (mean) before FFT to reveal oscillations
      // Rationale:
      //   - Raw FFT of μ(t) will have huge spike at k=0 (DC component)
      //   - This DC spike = non-zero mean bias (already visible in time plot)
      //   - It masks actual oscillations (e.g., 50Hz hum, mechanical vibration)
      //
      // After mean removal:
      //   - Peaks in FFT show periodic variations around the bias
      //   - Example: Peak at f=0.01 means 100-scan oscillation period
      //   - Can diagnose: temperature cycling, pump pulsation, line noise
      double mean = 0.0;
      for ( const double val : timeSeries )
      {
         mean += val;
      }
      mean /= N;

      data_to_transform.resize( N );
      for ( int i = 0; i < N; i++ )
      {
         data_to_transform[i] = timeSeries[i] - mean;
      }
   }

   // Simple DFT implementation (not optimized FFT, but sufficient for small N)
   // For production, consider using FFTW or Qt's built-in FFT if available
   const int halfN = N / 2;
   magnitude.resize( halfN );
   frequency.resize( halfN );

   for ( int k = 0; k < halfN; k++ )
   {
      double real_part = 0.0;
      double imag_part = 0.0;

      for ( int n = 0; n < N; n++ )
      {
         const double angle = -2.0 * M_PI * k * n / N;
         real_part += data_to_transform[n] * std::cos( angle );
         imag_part += data_to_transform[n] * std::sin( angle );
      }

      magnitude[k] = std::sqrt( real_part * real_part + imag_part * imag_part ) / N;
      frequency[k] = static_cast<double>( k ) / N;
   }
}

void US_StatsEngine::calculateFFTRaw( const QVector<double>& timeSeries, QVector<double>& magnitude,
                                      QVector<double>& frequency )
{
   calculateFFT( timeSeries, magnitude, frequency, false );
}

double US_StatsEngine::calculateHMetric( const QVector<double>& data, const double rmsd )
{
   if ( rmsd <= 0.0 || data.size() < 2 )
   {
      return 0.0;
   }

   // Find min and max
   const double r_min = *std::min_element( data.begin(), data.end() );
   const double r_max = *std::max_element( data.begin(), data.end() );

   const double span = r_max - r_min;
   if ( span <= 0.0 )
   {
      return 0.0;
   }

   // Determine the number of bins (aim for 100-200 bins, or adjust based on data size)
   const int n_points = data.size();
   int       n_bins   = 100;
   if ( n_points > 10000 )
   {
      n_bins = qMin( 500, n_points / 100 );
   }

   const double bin_wid   = span / n_bins;
   const double bin_start = r_min;

   // Count observed frequencies
   QVector<int> counts( n_bins, 0 );
   for ( const double val : data )
   {
      int bin = static_cast<int>( ( val - bin_start ) / bin_wid );
      bin     = qBound( 0, bin, n_bins - 1 );
      counts[bin]++;
   }

   // Calculate expected frequencies using normal distribution
   // f(x) = (1 / (rmsd * sqrt(2*pi))) * exp( -0.5 * (x/rmsd)^2 )
   // Expected count in bin is N * f(x_mid) * bin_wid
   const double constant = static_cast<double>( n_points ) * bin_wid / ( rmsd * std::sqrt( 2.0 * M_PI ) );

   double h_num = 0.0;
   double h_den = 0.0;

   for ( int i = 0; i < n_bins; i++ )
   {
      const double x_mid = bin_start + ( static_cast<double>( i ) + 0.5 ) * bin_wid;
      const double y_exp = constant * std::exp( -0.5 * std::pow( x_mid / rmsd, 2.0 ) );
      const auto   y_obs = static_cast<double>( counts[i] );

      const double diff = y_obs - y_exp;
      h_num += ( diff * diff );
      h_den += ( y_exp * y_exp );
   }

   return ( h_den > 0.0 ) ? ( h_num / h_den ) : 0.0;
}

US_StatsEngine::GlobalMetrics::GlobalMetrics() {
   excessKurtosis = NAN;
   ksStatistic    = NAN;
   hMetric        = NAN;
   durbin_watson  = NAN;
   valid          = false;
   errorMessage   = "";
}

US_StatsEngine::GlobalMetrics US_StatsEngine::calculateGlobalMetrics( const QVector<double>& data_flat )
{
   GlobalMetrics metrics = GlobalMetrics();


   // Filter non-finite values
   QVector<double> filtered = filterFiniteValues( data_flat );

   if ( filtered.size() < 2 )
   {
      metrics.errorMessage = "Insufficient finite data points";
      return metrics;
   }

   // Calculate mean and standard deviation
   double sum = 0.0;
   for ( const double val : filtered )
   {
      sum += val;
   }

   const double mean = sum / filtered.size();

   double variance = 0.0;
   for ( const double val : filtered )
   {
      const double diff = val - mean;
      variance += diff * diff;
   }

   variance /= filtered.size();
   const double stddev = std::sqrt( variance );

   if ( stddev <= 1e-15 )
   {
      metrics.errorMessage = "Zero variance (all values identical)";
      return metrics;
   }

   // Calculate global metrics
   metrics.excessKurtosis = calculateExcessKurtosis( filtered, mean, stddev );
   metrics.ksStatistic    = calculateKSStatistic( filtered, mean, stddev );
   metrics.hMetric        = calculateHMetric( filtered, stddev );
   metrics.durbin_watson  = calculateDurbinWatson( filtered );
   metrics.valid          = true;

   return metrics;
}

US_StatsEngine::TemporalMetrics
   US_StatsEngine::calculateTemporalMetrics( const QVector<QVector<double>>& residual_matrix )
{
   TemporalMetrics metrics;
   metrics.durbin_watson = NAN;
   metrics.globalRMSD    = NAN;
   metrics.valid         = false;

   const int nScans = residual_matrix.size();
   if ( nScans < 2 )
   {
      return metrics;
   }

   const int nPositions = residual_matrix[0].size();
   if ( nPositions < 1 )
   {
      return metrics;
   }

   // Calculate row-wise statistics (collapse spatial dimension)
   metrics.meanPerScan.resize( nScans );
   metrics.rmsdPerScan.resize( nScans );

   for ( int t = 0; t < nScans; t++ )
   {
      double sum    = 0.0;
      double sum_sq = 0.0;
      int    count  = 0;

      for ( int s = 0; s < nPositions; s++ )
      {
         const double val = residual_matrix[t][s];
         if ( std::isfinite( val ) )
         {
            sum += val;
            sum_sq += val * val;
            count++;
         }
      }

      if ( count > 0 )
      {
         metrics.meanPerScan[t] = sum / count;
         metrics.rmsdPerScan[t] = std::sqrt( sum_sq / count );
      }
      else
      {
         metrics.meanPerScan[t] = 0.0;
         metrics.rmsdPerScan[t] = 0.0;
      }
   }

   // Calculate Durbin-Watson on mean time series
   metrics.durbin_watson = calculateDurbinWatson( metrics.meanPerScan );

   // Calculate global RMSD across all scans
   double sum_rmsd = 0.0;
   for ( const double rmsd : metrics.rmsdPerScan )
   {
      sum_rmsd += rmsd;
   }
   metrics.globalRMSD = sum_rmsd / nScans;

   // Calculate FFT on mean time series (both with and without DC removal)
   calculateFFT( metrics.meanPerScan, metrics.fftMagnitude, metrics.fftFrequency, true ); // DC removed
   calculateFFTRaw( metrics.meanPerScan, metrics.fftMagnitudeRaw, metrics.fftFrequency ); // With DC

   metrics.valid = true;
   return metrics;
}

US_StatsEngine::SpatialMetrics
   US_StatsEngine::calculateSpatialMetrics( const QVector<QVector<double>>& residual_matrix )
{
   SpatialMetrics metrics;
   metrics.spatialVariance = NAN;
   metrics.valid           = false;

   const int nScans = residual_matrix.size();
   if ( nScans < 1 )
   {
      return metrics;
   }

   const int nPositions = residual_matrix[0].size();
   if ( nPositions < 2 )
   {
      return metrics;
   }

   // Calculate column-wise statistics (collapse temporal dimension)
   metrics.meanPerPosition.resize( nPositions );
   metrics.stddevPerPosition.resize( nPositions );

   for ( int s = 0; s < nPositions; s++ )
   {
      double sum   = 0.0;
      int    count = 0;

      // First pass: calculate mean
      for ( int t = 0; t < nScans; t++ )
      {
         const double val = residual_matrix[t][s];
         if ( std::isfinite( val ) )
         {
            sum += val;
            count++;
         }
      }

      const double mean          = ( count > 0 ) ? ( sum / count ) : 0.0;
      metrics.meanPerPosition[s] = mean;

      // Second pass: calculate stddev
      double var_sum = 0.0;
      for ( int t = 0; t < nScans; t++ )
      {
         const double val = residual_matrix[t][s];
         if ( std::isfinite( val ) )
         {
            const double diff = val - mean;
            var_sum += diff * diff;
         }
      }

      metrics.stddevPerPosition[s] = ( count > 1 ) ? std::sqrt( var_sum / ( count - 1 ) ) : 0.0;
   }

   // Calculate variance of spatial means
   double mean_of_means = 0.0;
   for ( const double m : metrics.meanPerPosition )
   {
      mean_of_means += m;
   }
   mean_of_means /= nPositions;

   double var_spatial = 0.0;
   for ( const double m : metrics.meanPerPosition )
   {
      const double diff = m - mean_of_means;
      var_spatial += diff * diff;
   }
   metrics.spatialVariance = var_spatial / nPositions;

   metrics.valid = true;
   return metrics;
}

// Perform SVD decomposition and extract principal modes
//US_StatsEngine::SVDResults US_StatsEngine::calculateSVD( const QVector<QVector<double>>& residual_matrix,
//                                                         const int                       n_modes )
//{
//   SVDResults results;
//   results.valid = false;
//
//   const int n_scans = residual_matrix.size();
//   if ( n_scans < 2 )
//   {
//      results.errorMessage = "Insufficient scans for SVD";
//      return results;
//   }
//
//   const int n_positions = residual_matrix[0].size();
//   if ( n_positions < 2 )
//   {
//      results.errorMessage = "Insufficient positions for SVD";
//      return results;
//   }
//
//   // Convert QVector to Eigen matrix
//   Eigen::MatrixXd R( n_scans, n_positions );
//   for ( int t = 0; t < n_scans; t++ )
//   {
//      for ( int s = 0; s < n_positions; s++ )
//      {
//         const double val = residual_matrix[t][s];
//         R( t, s )        = std::isfinite( val ) ? val : 0.0; // Replace NaN/Inf with 0
//      }
//   }
//
//   // Perform SVD: R = U * S * V^T
//   // Use BDC (Bidiagonal Divide and Conquer) for performance on large matrices
//   const Eigen::BDCSVD<Eigen::MatrixXd> svd( R, Eigen::ComputeThinU | Eigen::ComputeThinV );
//
//   Eigen::VectorXd singular_values = svd.singularValues();
//   Eigen::MatrixXd U               = svd.matrixU(); // Temporal components [nScans x min(nScans,nPositions)]
//   Eigen::MatrixXd V               = svd.matrixV(); // Spatial components [nPositions x min(nScans,nPositions)]
//
//   const int n_available = std::min( n_scans, n_positions );
//   const int n_extract   = std::min( n_modes, n_available );
//
//   // Calculate total variance for fraction computation
//   double total_variance = 0.0;
//   for ( int i = 0; i < singular_values.size(); i++ )
//   {
//      total_variance += singular_values( i ) * singular_values( i );
//   }
//
//   // Store all singular values for scree plot (up to 10)
//   const int n_scree = std::min( 10, n_available );
//   results.all_singular_values.resize( n_scree );
//   for ( int i = 0; i < n_scree; i++ )
//   {
//      results.all_singular_values[i] = singular_values( i );
//   }
//
//   // Extract top n_modes
//   results.modes.resize( n_extract );
//
//   for ( int k = 0; k < n_extract; k++ )
//   {
//      SVDMode mode;
//      mode.singular_value    = singular_values( k );
//      mode.variance_fraction = ( singular_values( k ) * singular_values( k ) ) / total_variance;
//
//      // Extract spatial vector (V[:,k]) - normalized shape
//      mode.spatial_vector.resize( n_positions );
//      for ( int s = 0; s < n_positions; s++ )
//      {
//         mode.spatial_vector[s] = V( s, k );
//      }
//
//      // Extract temporal vector (U[:,k]) and scale by singular value
//      // This gives the true magnitude in physical units
//      mode.temporal_vector.resize( n_scans );
//      for ( int t = 0; t < n_scans; t++ )
//      {
//         mode.temporal_vector[t] = U( t, k ) * singular_values( k );
//      }
//
//      // Deterministic sign convention:
//      // Find the element with the largest absolute magnitude in the spatial vector.
//      // If that element is negative, multiply both spatial and temporal vectors by -1.
//      // This ensures the "loudest" part of the spatial error is always positive.
//      double max_abs_val        = 0.0;
//      double element_at_max_abs = 0.0;
//
//      for ( const double val : mode.spatial_vector )
//      {
//         const double abs_val = std::abs( val );
//         if ( abs_val > max_abs_val )
//         {
//            max_abs_val        = abs_val;
//            element_at_max_abs = val;
//         }
//      }
//
//      if ( element_at_max_abs < 0.0 )
//      {
//         // Flip signs of both vectors
//         for ( int s = 0; s < n_positions; s++ )
//         {
//            mode.spatial_vector[s] = -mode.spatial_vector[s];
//         }
//         for ( int t = 0; t < n_scans; t++ )
//         {
//            mode.temporal_vector[t] = -mode.temporal_vector[t];
//         }
//      }
//
//      results.modes[k] = mode;
//   }
//
//   results.valid = true;
//   return results;
//}
