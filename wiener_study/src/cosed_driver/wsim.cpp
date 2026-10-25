// Wiener-skewing sensitivity study for UltraScan 2DSA fitting.
// Simulation: US_Astfem_RSA (ideal, no concentration dependence).
// Fitting:    2DSA-style subgrid NNLS + final NNLS, US_SolveSim::nnls_noise
//             (TI/RI noise removed algebraically as in UltraScan).
#include <QtCore>
#include <random>
#include <thread>
#include <mutex>
#include <cstdio>
#include "us_astfem_math.h"
#include "us_astfem_rsa.h"
#include "us_dataIO.h"
#include "us_model.h"
#include "us_simparms.h"
#include "us_solve_sim.h"
#include "us_constants.h"
#include "us_math2.h"
#include "us_lamm_astfvm.h"
#include "us_buffer.h"

struct Species { QString name; double s; double mw; double frac; };

struct Case {
   QString id, desc;
   int     rpm;
   double  meniscus, bottom, x0, x1;   // cell + fitted data range (cm)
   double  t0, t1;                     // first/last scan time (s)
   double  ctot;                       // total signal (fringes)
   double  kappa;                      // Wiener skew coefficient (cm^2/fringe)
   double  vbar;
   double  smin, smax;                 // 2DSA s range (S)
   QVector<Species> sp;
};

static const int    NSCANS   = 60;
static const double DR_FULL  = 0.0005;   // truth / skew grid (cm)
static const int    DECIM    = 2;        // fit grid = every 2nd point (0.001 cm)
static const int    SIMPTS_TRUE = 1000;
static const int    SIMPTS_FIT  = 200;   // UltraScan default
static int          NGS = 113, NGK = 31;  // 2DSA grid (s x f/f0)
static const double KMIN = 1.0, KMAX = 4.0;
static int          SUBS = 8, SUBK = 4;  // subgrid interleave factors (24 subgrids)
static int          NTHREADS = 4;

// ---------------------------------------------------------------------------
struct Setup {
   US_SimulationParameters sp;
   US_DataIO::EditedData   ed;
   QString                 tmst;
};

static void build_setup( Setup& su, const Case& cs, const QVector<double>& x,
                         int simpts, const QString& tmst, bool write_ts )
{
   US_SimulationParameters& sp = su.sp;
   US_SimulationParameters::SpeedProfile st;
   st.rotorspeed = cs.rpm; st.avg_speed = cs.rpm; st.set_speed = cs.rpm;
   st.scans = NSCANS; st.acceleration = 400; st.acceleration_flag = true;
   st.delay_hours = 0; st.delay_minutes = cs.t0 / 60.0;
   st.duration_hours = 0; st.duration_minutes = cs.t1 / 60.0;
   sp.meniscus = cs.meniscus; sp.bottom = cs.bottom; sp.bottom_position = cs.bottom;
   sp.sim = true; sp.simpoints = simpts;
   sp.rotorcoeffs[0] = 0.0; sp.rotorcoeffs[1] = 0.0;

   US_DataIO::RawData rd;
   rd.type[0] = 'I'; rd.type[1] = 'P';
   memset( rd.rawGUID, 0, sizeof( rd.rawGUID ) );
   rd.cell = 1; rd.channel = 'A'; rd.description = cs.id;
   rd.xvalues = x;
   double tinc = ( cs.t1 - cs.t0 ) / ( NSCANS - 1 );
   for ( int s = 0; s < NSCANS; s++ ) {
      US_DataIO::Scan sc;
      sc.temperature = NORMAL_TEMP; sc.rpm = cs.rpm;
      sc.seconds = (double) qRound( cs.t0 + tinc * s );
      sc.omega2t = US_AstfemMath::calc_omega2t( 0.0, 0.0, 0.0, cs.rpm, 400, sc.seconds );
      sc.wavelength = 660.0; sc.plateau = 0.0; sc.delta_r = x[1] - x[0];
      sc.nz_stddev = false; sc.rvalues.fill( 0.0, x.size() );
      rd.scanData << sc;
   }
   st.time_first = qRound( rd.scanData.first().seconds );
   st.w2t_first  = rd.scanData.first().omega2t;
   st.time_last  = qRound( rd.scanData.last().seconds );
   st.w2t_last   = rd.scanData.last().omega2t;
   sp.speed_step.clear(); sp.speed_step << st;
   if ( write_ts ) US_AstfemMath::writetimestate( tmst, sp, rd );
   sp.simSpeedsFromTimeState( tmst );
   su.tmst = tmst;

   US_DataIO::EditedData& ed = su.ed;
   ed.expType = "velocity"; ed.runID = cs.id; ed.editID = "0"; ed.dataType = "IP";
   ed.cell = "1"; ed.channel = "A"; ed.wavelength = "660"; ed.description = cs.id;
   ed.meniscus = cs.meniscus; ed.bottom = cs.bottom; ed.plateau = cs.x1;
   ed.baseline = 0.0; ed.ODlimit = 1e20; ed.floatingData = false;
   ed.xvalues = rd.xvalues; ed.scanData = rd.scanData;
   ed.bl_corr_slope = 0.0; ed.bl_corr_yintercept = 0.0;
}

// Simulate unit-concentration single component; returns nscans*npoints
static QVector<double> simulate1( Setup& su, double s, double k, double vbar, double conc )
{
   US_Model m;
   US_Model::SimulationComponent c;
   c.s = s * 1e-13; c.f_f0 = k; c.vbar20 = vbar; c.mw = 0; c.D = 0; c.f = 0;
   c.signal_concentration = conc;
   US_Model::calc_coefficients( c );
   m.components << c;
   m.coSedSolute = -1;
   US_DataIO::RawData sd;
   US_AstfemMath::initSimData( sd, su.ed, 0.0 );
   US_Astfem_RSA rsa( m, su.sp );
   rsa.calculate( sd );
   int np = sd.pointCount(), ns = sd.scanCount();
   QVector<double> v( ns * np );
   for ( int i = 0; i < ns; i++ )
      for ( int j = 0; j < np; j++ ) v[ i * np + j ] = sd.value( i, j );
   return v;
}

static QVector<double> simulate1_fvm( Setup& su, double s, double k, double vbar, double sigma, double delta )
{
   US_Model m;
   US_Model::SimulationComponent c;
   c.s = s * 1e-13; c.f_f0 = k; c.vbar20 = vbar; c.mw = 0; c.D = 0; c.f = 0;
   c.signal_concentration = 1.0;           // as US_SolveSim (cosedimenting) basis functions
   US_Model::calc_coefficients( c );
   c.sigma = sigma; c.delta = delta;
   m.components << c;
   m.coSedSolute = -1;
   US_DataIO::RawData sd;
   US_AstfemMath::initSimData( sd, su.ed, 0.0 );
   su.sp.meshType = US_SimulationParameters::ASTFVM;
   su.sp.sigma = sigma; su.sp.delta = delta; su.sp.temperature = NORMAL_TEMP;
   US_Buffer buf; buf.density = DENS_20W; buf.viscosity = VISC_20W; buf.compressibility = 0.0; buf.manual = true;
   US_LammAstfvm fvm( m, su.sp );
   fvm.set_buffer( buf, nullptr, nullptr );
   fvm.calculate( sd );
   int np = sd.pointCount(), ns = sd.scanCount();
   QVector<double> v( ns * np );
   for ( int i = 0; i < ns; i++ )
      for ( int j = 0; j < np; j++ ) v[ i * np + j ] = sd.value( i, j );
   return v;
}

static double kfrom_mw( double s, double mw, double vbar )
{
   US_Model::SimulationComponent c;
   c.s = s * 1e-13; c.mw = mw; c.vbar20 = vbar; c.D = 0; c.f = 0; c.f_f0 = 0;
   US_Model::calc_coefficients( c );
   return c.f_f0;
}

// Parallel basis simulation (per-thread Setup copies, own TimeState objects)
static void simulate_many( const Case& cs, const QVector<double>& x, int simpts, const QString& tmst,
                           const QVector<US_Solute>& sols, QVector<QVector<double>>& out )
{
   out.resize( sols.size() );
   std::atomic<int> next{0};
   auto work = [&]() {
      Setup su; build_setup( su, cs, x, simpts, tmst, false );
      for ( int i; ( i = next++ ) < sols.size(); )
         out[ i ] = simulate1( su, sols[i].s, sols[i].k, cs.vbar, 1.0 );
   };
   std::vector<std::thread> th;
   (void) th; work();   // serial: US_Astfem_RSA is not thread-safe in-process
}

// ---------------------------------------------------------------------------
// Wiener skew: object point r imaged at rho = r + kappa * dc/dr (c = macromolecular
// signal only); observed value at detector position = object signal u(r(rho)).
static QVector<double> skew_scan( const QVector<double>& x, const double* c, const double* u,
                                  double kappa, double& maxshift, int& nonmono, double xa, double xb )
{
   int n = x.size();
   QVector<double> rho( n ), out( n );
   for ( int i = 0; i < n; i++ ) {
      double g;
      if ( i == 0 ) g = ( c[1] - c[0] ) / ( x[1] - x[0] );
      else if ( i == n - 1 ) g = ( c[n-1] - c[n-2] ) / ( x[n-1] - x[n-2] );
      else g = ( c[i+1] - c[i-1] ) / ( x[i+1] - x[i-1] );
      rho[i] = x[i] + kappa * g;
      if ( x[i] >= xa && x[i] <= xb ) maxshift = qMax( maxshift, fabs( kappa * g ) );
   }
   for ( int i = 1; i < n; i++ ) if ( rho[i] <= rho[i-1] && x[i] >= xa - 0.01 && x[i] <= xb + 0.01 ) nonmono++;
   // interpolate u as function of rho onto x (assumes monotonic; else nearest-bracket)
   int j = 0;
   for ( int i = 0; i < n; i++ ) {
      double xi = x[i];
      if ( xi <= rho[0] ) { out[i] = u[0]; continue; }
      if ( xi >= rho[n-1] ) { out[i] = u[n-1]; continue; }
      while ( j < n - 2 && rho[j+1] < xi ) j++;
      while ( j > 0 && rho[j] > xi ) j--;
      double w = ( xi - rho[j] ) / ( rho[j+1] - rho[j] );
      out[i] = u[j] + w * ( u[j+1] - u[j] );
   }
   return out;
}

// ---------------------------------------------------------------------------
struct FitResult {
   QVector<US_Solute> sol; QVector<double> ti, ri; double rmsd; QVector<double> resid;
};

static FitResult nnls_fit( const QVector<QVector<double>>& A, const QVector<int>& cols,
                           const QVector<double>& b, int ns, int np, int noisflag,
                           const QVector<US_Solute>& grid )
{
   int nr = ns * np, nc = cols.size();
   QVector<double> a( nr * nc ), bb = b, xx( nc, 0.0 ), ti( np, 0.0 ), ri( ns, 0.0 );
   for ( int c = 0; c < nc; c++ )
      std::copy( A[ cols[c] ].begin(), A[ cols[c] ].end(), a.begin() + c * nr );
   QVector<int> dsc{ ns }, dpt{ np };
   // Same projection as US_SolveSim::nnls_noise (TI: radius means over scans,
   // RI: scan means), but with a larger NNLS iteration cap than its 3*n default.
   auto center = [&]( double* v, double* rmean, double* smean ) {
      if ( noisflag & 1 ) for ( int p = 0; p < np; p++ ) { double m = 0; for ( int s = 0; s < ns; s++ ) m += v[ s*np+p ]; m /= ns;
         if ( rmean ) rmean[p] = m; for ( int s = 0; s < ns; s++ ) v[ s*np+p ] -= m; }
      if ( noisflag & 2 ) for ( int s = 0; s < ns; s++ ) { double m = 0; for ( int p = 0; p < np; p++ ) m += v[ s*np+p ]; m /= np;
         if ( smean ) smean[s] = m; for ( int p = 0; p < np; p++ ) v[ s*np+p ] -= m; }
   };
   QVector<double> abar( np, 0.0 ), atil( ns, 0.0 ), Lbar( nc * np, 0.0 ), Ltil( nc * ns, 0.0 );
   if ( noisflag ) {
      center( bb.data(), abar.data(), atil.data() );
      for ( int c = 0; c < nc; c++ ) center( a.data() + (size_t) c * nr, Lbar.data() + c * np, Ltil.data() + c * ns );
   }
   int rc = US_Math2::nnls( a.data(), nr, nr, nc, bb.data(), xx.data(), nullptr, nullptr, nullptr, nullptr, 50 * nc, nullptr );
   if ( rc != 0 ) fprintf( stderr, "    nnls rc=%d ncols=%d\n", rc, nc );
   if ( noisflag & 1 ) for ( int p = 0; p < np; p++ ) { double v = abar[p]; for ( int c = 0; c < nc; c++ ) v -= xx[c] * Lbar[ c*np+p ]; ti[p] = v; }
   if ( noisflag & 2 ) for ( int s = 0; s < ns; s++ ) { double v = atil[s]; for ( int c = 0; c < nc; c++ ) v -= xx[c] * Ltil[ c*ns+s ]; ri[s] = v; }
   FitResult fr;
   fr.resid = b;
   for ( int c = 0; c < nc; c++ ) {
      if ( xx[c] <= 0.0 ) continue;
      US_Solute so = grid[ cols[c] ]; so.c = xx[c]; fr.sol << so;
      const QVector<double>& col = A[ cols[c] ];
      for ( int r = 0; r < nr; r++ ) fr.resid[r] -= xx[c] * col[r];
   }
   double ss = 0;
   for ( int s = 0; s < ns; s++ )
      for ( int p = 0; p < np; p++ ) {
         double& v = fr.resid[ s * np + p ];
         v -= ( ( noisflag & 1 ) ? ti[p] : 0.0 ) + ( ( noisflag & 2 ) ? ri[s] : 0.0 );
         ss += v * v;
      }
   fr.rmsd = sqrt( ss / nr ); fr.ti = ti; fr.ri = ri;
   return fr;
}

// 2DSA: interleaved subgrids, survivors pooled, 2 refinement iterations, final fit
static FitResult fit_2dsa( const QVector<QVector<double>>& A, const QVector<US_Solute>& grid,
                           const QVector<double>& b, int ns, int np, int noisflag )
{
   QVector<int> surv;
   FitResult fr;
   for ( int iter = 0; iter < 2; iter++ ) {
      QSet<int> pool;
      QVector<QVector<int>> subs;
      for ( int is = 0; is < SUBS; is++ ) for ( int ik = 0; ik < SUBK; ik++ ) {
         QVector<int> cols;
         for ( int s = is; s < NGS; s += SUBS ) for ( int k = ik; k < NGK; k += SUBK )
            cols << s * NGK + k;
         for ( int c : surv ) if ( !cols.contains( c ) ) cols << c;
         subs << cols;
      }
      std::mutex mx; std::atomic<int> next{0};
      auto work = [&]() {
         for ( int i; ( i = next++ ) < subs.size(); ) {
            FitResult f = nnls_fit( A, subs[i], b, ns, np, noisflag, grid );
            std::lock_guard<std::mutex> lk( mx );
            for ( auto& so : f.sol )
               for ( int c : subs[i] ) if ( grid[c].s == so.s && grid[c].k == so.k ) pool << c;
         }
      };
      std::vector<std::thread> th;
      for ( int t = 0; t < NTHREADS; t++ ) th.emplace_back( work );
      for ( auto& t : th ) t.join();
      QVector<int> cols( pool.begin(), pool.end() ); std::sort( cols.begin(), cols.end() );
      fr = nnls_fit( A, cols, b, ns, np, noisflag, grid );
      surv.clear();
      for ( auto& so : fr.sol ) for ( int c : cols ) if ( grid[c].s == so.s && grid[c].k == so.k ) surv << c;
   }
   return fr;
}

// ---------------------------------------------------------------------------
// Real (SEDFIT .ip) data: 2DSA uncorrected (iter 0) and C1 skew-corrected
// (iter 1..NIT, kappa' known). Config file: key=value lines.
static int run_real( const QString& outdir, const QString& cfgfile )
{
   QMap<QString, QString> cf;
   { QFile f( cfgfile ); f.open( QIODevice::ReadOnly );
     for ( QString l : QString( f.readAll() ).split( '\n' ) ) { l = l.trimmed(); int e = l.indexOf( '=' );
        if ( e > 0 && !l.startsWith( '#' ) ) cf[ l.left( e ).trimmed() ] = l.mid( e + 1 ).trimmed(); } }
   QString id = cf["id"], dir = cf["dir"];
   double meniscus = cf["meniscus"].toDouble(), bottom = cf["bottom"].toDouble();
   double x0 = cf["x0"].toDouble(), x1 = cf["x1"].toDouble(), kappa = cf["kappa"].toDouble();
   double smin = cf["smin"].toDouble(), smax = cf["smax"].toDouble(), vbar = cf.value( "vbar", "0.73" ).toDouble();
   double dr = cf.value( "dr", "0.0007" ).toDouble();
   int stride = cf.value( "stride", "1" ).toInt(), simpts = cf.value( "simpoints", "500" ).toInt();
   int NIT = cf.value( "nit", "6" ).toInt();
   QStringList menis = cf.value( "meniscus_scan", "" ).split( ',', Qt::SkipEmptyParts );
   bool   use_fvm = cf.value( "solver", "astfem" ) == "astfvm";
   double fsigma  = cf.value( "sigma", "0" ).toDouble(), fdelta = cf.value( "delta", "0" ).toDouble();
   NGS = cf.value( "ngs", QString::number( NGS ) ).toInt(); NGK = cf.value( "ngk", QString::number( NGK ) ).toInt();
   SUBS = cf.value( "subs", QString::number( SUBS ) ).toInt(); SUBK = cf.value( "subk", QString::number( SUBK ) ).toInt();
   if ( cf.contains( "tag" ) ) id += "_" + cf["tag"];

   // read list + scans
   QStringList files;
   { QFile f( dir + "/" + cf["list"] ); f.open( QIODevice::ReadOnly );
     QStringList L = QString( f.readAll() ).split( '\n' );
     for ( int i = 2; i < L.size(); i++ ) if ( !L[i].trimmed().isEmpty() ) files << L[i].trimmed(); }
   QVector<double> tsec, w2t; QVector<QVector<double>> rr, vv; int rpm = 0;
   for ( int i = 0; i < files.size(); i += stride ) {
      QFile f( dir + "/" + files[i] ); if ( !f.open( QIODevice::ReadOnly ) ) { fprintf( stderr, "missing %s\n", qPrintable( files[i] ) ); continue; }
      QStringList L = QString( f.readAll() ).split( '\n' );
      QStringList h = L[1].split( ' ', Qt::SkipEmptyParts );
      rpm = h[3].toInt(); tsec << h[4].toDouble(); w2t << h[5].toDouble();
      QVector<double> r, v;
      for ( int k = 2; k < L.size(); k++ ) { QStringList p = L[k].split( ' ', Qt::SkipEmptyParts );
         if ( p.size() >= 2 ) { r << p[0].toDouble(); v << p[1].toDouble(); } }
      rr << r; vv << v;
   }
   int ns = tsec.size();
   double om2 = sq( rpm * M_PI / 30.0 );
   double Ta = 1.5 * ( tsec[0] - w2t[0] / om2 );
   int acc = qBound( 50, qRound( rpm / qMax( Ta, 1.0 ) ), 2000 );
   fprintf( stderr, "%s: %d scans, rpm %d, ramp %.0f s -> accel %d rpm/s\n", qPrintable( id ), ns, rpm, Ta, acc );

   // grids
   QVector<double> xe; QVector<int> fsub;
   for ( double r = meniscus + 0.002; r <= qMin( x1 + 0.05, bottom - 0.005 ) + 1e-9; r += dr ) {
      if ( r >= x0 && r <= x1 ) fsub << xe.size();
      xe << r; }
   int npe = xe.size(), np = fsub.size();
   QVector<double> xfit; for ( int p : fsub ) xfit << xe[p];

   // data on fit grid (linear interpolation)
   QVector<double> b( ns * np );
   for ( int s = 0; s < ns; s++ ) { int j = 0; const QVector<double>& r = rr[s]; const QVector<double>& v = vv[s];
      for ( int p = 0; p < np; p++ ) { double x = xfit[p];
         while ( j < r.size() - 2 && r[j+1] < x ) j++;
         double w = qBound( 0.0, ( x - r[j] ) / ( r[j+1] - r[j] ), 1.0 );
         b[ s*np+p ] = v[j] + w * ( v[j+1] - v[j] ); } }

   // UltraScan simulation setup: scan times from headers, ramp from first scan
   auto make_setup = [&]( Setup& su, const QVector<double>& x, double meni, const QString& tmst, bool wts ) {
      US_SimulationParameters& sp = su.sp;
      US_SimulationParameters::SpeedProfile st;
      st.rotorspeed = rpm; st.avg_speed = rpm; st.set_speed = rpm; st.scans = ns;
      st.acceleration = acc; st.acceleration_flag = true;
      st.delay_hours = 0; st.delay_minutes = tsec[0] / 60.0; st.duration_hours = 0; st.duration_minutes = tsec[ns-1] / 60.0;
      sp.meniscus = meni; sp.bottom = bottom; sp.bottom_position = bottom; sp.sim = true; sp.simpoints = simpts;
      sp.rotorcoeffs[0] = 0.0; sp.rotorcoeffs[1] = 0.0;
      US_DataIO::RawData rd; rd.type[0] = 'I'; rd.type[1] = 'P'; memset( rd.rawGUID, 0, sizeof( rd.rawGUID ) );
      rd.cell = 1; rd.channel = 'A'; rd.description = id; rd.xvalues = x;
      for ( int s = 0; s < ns; s++ ) { US_DataIO::Scan sc; sc.temperature = NORMAL_TEMP; sc.rpm = rpm; sc.seconds = tsec[s];
         sc.omega2t = US_AstfemMath::calc_omega2t( 0.0, 0.0, 0.0, rpm, acc, sc.seconds ); sc.wavelength = 655; sc.plateau = 0;
         sc.delta_r = dr; sc.nz_stddev = false; sc.rvalues.fill( 0.0, x.size() ); rd.scanData << sc; }
      st.time_first = qRound( tsec[0] ); st.w2t_first = rd.scanData.first().omega2t;
      st.time_last = qRound( tsec[ns-1] ); st.w2t_last = rd.scanData.last().omega2t;
      sp.speed_step.clear(); sp.speed_step << st;
      if ( wts ) US_AstfemMath::writetimestate( tmst, sp, rd );
      sp.simSpeedsFromTimeState( tmst );
      US_DataIO::EditedData& ed = su.ed;
      ed.expType = "velocity"; ed.runID = id; ed.editID = "0"; ed.dataType = "IP"; ed.cell = "1"; ed.channel = "A";
      ed.wavelength = "655"; ed.description = id; ed.meniscus = meni; ed.bottom = bottom; ed.plateau = x1; ed.baseline = 0;
      ed.ODlimit = 1e20; ed.floatingData = false; ed.xvalues = rd.xvalues; ed.scanData = rd.scanData;
      ed.bl_corr_slope = 0; ed.bl_corr_yintercept = 0;
      double mx = 0; for ( int s = 0; s < ns; s++ ) mx = qMax( mx, fabs( rd.scanData[s].omega2t / w2t[s] - 1.0 ) );
      return mx; };

   QVector<US_Solute> grid;
   for ( int is = 0; is < NGS; is++ ) for ( int ik = 0; ik < NGK; ik++ )
      grid << US_Solute( smin + ( smax - smin ) * is / ( NGS - 1 ), KMIN + ( KMAX - KMIN ) * ik / ( NGK - 1 ), 0.0 );
   auto gidx = [&]( const US_Solute& so ) {
      return qRound( ( so.s - smin ) / ( smax - smin ) * ( NGS - 1 ) ) * NGK + qRound( ( so.k - KMIN ) / ( KMAX - KMIN ) * ( NGK - 1 ) ); };
   auto sim_basis = [&]( double meni, QVector<QVector<float>>& Ae ) {
      QString tmst = outdir + "/" + id + ".time_state.tmst";
      Setup su; double mx = make_setup( su, xe, meni, tmst, true );
      fprintf( stderr, "  meniscus %.4f: max |w2t_sim/w2t_data - 1| = %.2e\n", meni, mx );
      Ae.resize( grid.size() );
      for ( int c = 0; c < grid.size(); c++ ) {
         QVector<double> v = use_fvm ? simulate1_fvm( su, grid[c].s, grid[c].k, vbar, fsigma, fdelta )
                                     : simulate1( su, grid[c].s, grid[c].k, vbar, 1.0 );
         Ae[c].resize( v.size() ); for ( int i = 0; i < v.size(); i++ ) Ae[c][i] = (float) v[i]; } };

   QVector<QVector<double>> A( grid.size(), QVector<double>( ns * np ) );
   QVector<int> sj( ns * np ); QVector<double> sw( ns * np );
   auto build = [&]( const QVector<QVector<float>>& Ae, const QVector<US_Solute>& sol, double kap, double& maxdr ) {
      maxdr = 0; QVector<double> cfv( npe ), rho( npe );
      for ( int s = 0; s < ns; s++ ) {
         cfv.fill( 0.0 );
         for ( auto& so : sol ) { const float* a = Ae[ gidx( so ) ].constData() + s * npe; for ( int i = 0; i < npe; i++ ) cfv[i] += so.c * a[i]; }
         for ( int i = 0; i < npe; i++ ) { int i0 = qMax( 0, i - 1 ), i1 = qMin( npe - 1, i + 1 );
            double d = kap * ( cfv[i1] - cfv[i0] ) / ( xe[i1] - xe[i0] ); rho[i] = xe[i] + d;
            if ( i > 0 && rho[i] < rho[i-1] ) rho[i] = rho[i-1] + 1e-9;
            if ( xe[i] >= x0 && xe[i] <= x1 ) maxdr = qMax( maxdr, fabs( d ) ); }
         int j = 0;
         for ( int p = 0; p < np; p++ ) { double x = xfit[p];
            while ( j < npe - 2 && rho[j+1] < x ) j++;
            while ( j > 0 && rho[j] > x ) j--;
            double w = ( x <= rho[0] ) ? 0.0 : ( x >= rho[npe-1] ? 1.0 : ( x - rho[j] ) / ( rho[j+1] - rho[j] ) );
            if ( x >= rho[npe-1] ) j = npe - 2;
            sj[ s*np+p ] = j; sw[ s*np+p ] = qBound( 0.0, w, 1.0 ); } }
      std::atomic<int> next{0};
      auto work = [&]() { for ( int c; ( c = next++ ) < grid.size(); ) { const float* a = Ae[c].constData(); double* o = A[c].data();
         for ( int s = 0; s < ns; s++ ) for ( int p = 0; p < np; p++ ) { int k = s*np+p, j = sj[k]; double w = sw[k];
            o[k] = ( 1.0 - w ) * a[ s*npe + j ] + w * a[ s*npe + j + 1 ]; } } };
      std::vector<std::thread> th; for ( int t = 0; t < 4; t++ ) th.emplace_back( work ); for ( auto& t : th ) t.join(); };

   QJsonObject cj; cj["id"] = id; cj["kappa"] = kappa; cj["sigma"] = fsigma; cj["delta"] = fdelta; cj["solver"] = use_fvm ? "astfvm" : "astfem"; cj["ns"] = ns; cj["np"] = np;
   QJsonArray xs; for ( double v : xfit ) xs << v; cj["x"] = xs;
   QJsonArray ts; for ( double v : tsec ) ts << v; cj["t"] = ts;
   QJsonArray fits;
   QVector<QVector<float>> Ae;
   double meni_best = meniscus;
   if ( !menis.isEmpty() ) {   // meniscus scan (uncorrected 2DSA), as UltraScan's meniscus fit
      double best = 1e99;
      for ( auto& m : menis ) { double mv = m.toDouble(); sim_basis( mv, Ae ); double md;
         build( Ae, {}, 0.0, md ); FitResult fr = fit_2dsa( A, grid, b, ns, np, 3 );
         fprintf( stderr, "  meniscus %.4f rmsd %.5f\n", mv, fr.rmsd );
         QJsonObject o; o["meniscus"] = mv; o["rmsd"] = fr.rmsd; fits << o;
         if ( fr.rmsd < best ) { best = fr.rmsd; meni_best = mv; } }
      cj["meniscus_scan"] = fits; fits = QJsonArray();
   }
   cj["meniscus"] = meni_best;
   sim_basis( meni_best, Ae );
   QVector<US_Solute> sol;
   for ( int it = 0; it <= NIT; it++ ) {
      double md; build( Ae, sol, it == 0 ? 0.0 : kappa, md );
      QElapsedTimer et; et.start();
      FitResult fr = fit_2dsa( A, grid, b, ns, np, 3 );
      sol = fr.sol;
      fprintf( stderr, "  iter %d: rmsd %.5f nsol %d maxdr %.1f um (%.1f s)\n", it, fr.rmsd, fr.sol.size(), md * 1e4, et.elapsed() / 1000.0 );
      QJsonObject o; o["iter"] = it; o["rmsd"] = fr.rmsd; o["maxdr"] = md;
      QJsonArray sa; for ( auto& so : fr.sol ) sa << QJsonArray{ so.s, so.k, so.c }; o["solutes"] = sa;
      QJsonArray ta; for ( double v : fr.ti ) ta << v; o["ti"] = ta;
      fits << o;
      if ( it == 0 || it == NIT ) {
         QFile f( outdir + "/" + id + "_it" + QString::number( it ) + "_resid.bin" ); f.open( QIODevice::WriteOnly );
         f.write( (const char*) fr.resid.constData(), fr.resid.size() * 8 );
         QVector<double> fit( ns * np ); for ( int i = 0; i < ns * np; i++ ) fit[i] = b[i] - fr.resid[i];
         QFile g( outdir + "/" + id + "_it" + QString::number( it ) + "_fit.bin" ); g.open( QIODevice::WriteOnly );
         g.write( (const char*) fit.constData(), fit.size() * 8 );
      }
      if ( kappa == 0.0 ) break;
   }
   { QFile f( outdir + "/" + id + "_data.bin" ); f.open( QIODevice::WriteOnly ); f.write( (const char*) b.constData(), b.size() * 8 ); }
   cj["fits"] = fits;
   QFile jf( outdir + "/" + id + "_real.json" ); jf.open( QIODevice::WriteOnly );
   jf.write( QJsonDocument( cj ).toJson( QJsonDocument::Compact ) );
   return 0;
}

// ---------------------------------------------------------------------------
int main( int argc, char** argv )
{
   QCoreApplication app( argc, argv );
   if ( getenv( "WQUIET" ) ) qInstallMessageHandler( []( QtMsgType ty, const QMessageLogContext&, const QString& m ) { if ( ty != QtDebugMsg ) fprintf( stderr, "%s\n", qPrintable( m ) ); } );
   QString outdir = argc > 1 ? argv[1] : "out";
   QString only   = argc > 2 ? argv[2] : "";
   NTHREADS = getenv( "WTHREADS" ) ? atoi( getenv( "WTHREADS" ) ) : qMax( 1, (int) std::thread::hardware_concurrency() );
   QDir().mkpath( outdir );

   // Signal increments (Table 1): 12 mm 3.3, 1 mm 0.275 fringes/(mg/ml)
   // kappa' (Table 1): 12 mm 1.51e-5, 1 mm 2.69e-5 cm^2/fringe
   double vb = 0.73;
   QVector<Case> cases;
   cases << Case{ "IgG10_12mm", "hIgG 10 mg/ml, 12 mm, 45k rpm (Fig. 5a)", 45000, 6.05, 7.20, 6.08, 7.00,
                  300, 11000, 10 * 3.3, 1.51e-5, vb, 1, 15,
                  { { "frag", 3.5, 50e3, 0.03 }, { "monomer", 6.4, 150e3, 0.77 },
                    { "dimer", 9.3, 300e3, 0.16 }, { "trimer", 11.8, 450e3, 0.04 } } };
   cases << Case{ "IgG46_1mm", "hIgG 46 mg/ml, 1 mm, 45k rpm (Fig. 5b)", 45000, 6.05, 7.20, 6.08, 7.00,
                  300, 11000, 46 * 0.275, 2.69e-5, vb, 1, 15,
                  { { "frag", 3.5, 50e3, 0.03 }, { "monomer", 6.4, 150e3, 0.77 },
                    { "dimer", 9.3, 300e3, 0.16 }, { "trimer", 11.8, 450e3, 0.04 } } };
   cases << Case{ "IgG50_1mm", "IgG 50 mg/ml, 1 mm, 45k rpm, single species (Fig. 3)", 45000, 6.05, 7.20, 6.08, 7.00,
                  300, 11000, 50 * 0.275, 2.69e-5, vb, 1, 15,
                  { { "monomer", 6.3, 150e3, 1.00 } } };
   cases << Case{ "BSA140_1mm", "BSA 140 mg/ml, 1 mm, 40k rpm (Fig. 6)", 40000, 5.85, 7.20, 5.88, 7.00,
                  300, 25000, 140 * 0.275, 2.69e-5, vb, 1, 12,
                  { { "monomer", 4.3, 66.4e3, 0.78 }, { "dimer", 6.6, 132.8e3, 0.19 },
                    { "trimer", 8.6, 199.2e3, 0.03 } } };

   struct NoiseCond { QString id; double rnd; bool tiri; int fitflag; int nseed; };
   QVector<NoiseCond> conds = { { "none", 0.0, false, 0, 1 }, { "TIRI_only", 0.0, true, 3, 1 },
                                { "r0.005", 0.005, true, 3, 3 }, { "r0.01", 0.01, true, 3, 3 },
                                { "r0.02", 0.02, true, 3, 3 } };

   if ( only == "REAL" ) return run_real( outdir, argc > 3 ? argv[3] : "" );
   if ( getenv( "WT0" ) ) for ( Case& cs : cases ) cs.t0 = atof( getenv( "WT0" ) );
   for ( const Case& cs : cases ) {
      if ( !only.isEmpty() && cs.id != only ) continue;
      fprintf( stderr, "=== %s\n", qPrintable( cs.id ) );
      QString tmst = outdir + "/" + cs.id + ".time_state.tmst";

      // grids
      QVector<double> xfull;
      for ( double r = cs.meniscus + 0.002; r <= cs.bottom - 0.002 + 1e-9; r += DR_FULL ) xfull << r;
      QVector<int> fidx; QVector<double> xfit;
      for ( int i = 0; i < xfull.size(); i += DECIM )
         if ( xfull[i] >= cs.x0 && xfull[i] <= cs.x1 ) { fidx << i; xfit << xfull[i]; }
      int nf = xfull.size(), np = xfit.size(), ns = NSCANS;
      { Setup tmp; build_setup( tmp, cs, xfull, SIMPTS_TRUE, tmst, true ); }

      // truth (high-resolution simulation, full cell)
      QVector<US_Solute> tsol;
      for ( auto& s : cs.sp ) tsol << US_Solute( s.s, kfrom_mw( s.s, s.mw, cs.vbar ), s.frac * cs.ctot );
      QVector<QVector<double>> tsim;
      simulate_many( cs, xfull, SIMPTS_TRUE, tmst, tsol, tsim );
      if ( getenv( "WDEBUG" ) ) {
         QVector<QVector<double>> b1, b2, b3;
         simulate_many( cs, xfit, SIMPTS_FIT, tmst, tsol, b1 );
         simulate_many( cs, xfit, SIMPTS_TRUE, tmst, tsol, b2 );
         simulate_many( cs, xfull, SIMPTS_FIT, tmst, tsol, b3 );
         for ( int s = 0; s < ns; s += 10 ) {
            double m1 = 0, m2 = 0, m3 = 0; int pm = 0;
            for ( int p = 0; p < np; p++ ) {
               double tv = tsim[0][ s * nf + fidx[p] ];
               if ( fabs( tv - b1[0][ s*np+p ] ) > m1 ) { m1 = fabs( tv - b1[0][ s*np+p ] ); pm = p; }
               m2 = qMax( m2, fabs( tv - b2[0][ s*np+p ] ) );
               m3 = qMax( m3, fabs( tv - b3[0][ s*nf+fidx[p] ] ) );
            }
            fprintf( stderr, "scan %d  |true-fit200| %.4f at r=%.3f  |true-fit1000grid| %.4f  |true-full200| %.4f  plateau %.4f\n",
                     s, m1, xfit[pm], m2, m3, tsim[0][ s*nf + fidx[np/2] ] );
         }
      }
      QVector<double> ctrue( ns * nf, 0.0 );
      for ( int k = 0; k < tsol.size(); k++ )
         for ( int i = 0; i < ns * nf; i++ ) ctrue[i] += tsol[k].c * tsim[k][i];

      // TI noise in object plane: smooth random walk + sharp blips (scratches)
      std::mt19937 rti( 12345 );
      std::normal_distribution<double> nd( 0.0, 1.0 );
      QVector<double> ti( nf );
      { double v = 0, sm = 0; for ( int i = 0; i < nf; i++ ) { v = 0.995 * v + nd( rti ); ti[i] = v; sm += v; }
        sm /= nf; double ss = 0; for ( auto& t : ti ) { t -= sm; ss += t * t; }
        double sc = 0.01 / sqrt( ss / nf ); for ( auto& t : ti ) t *= sc;
        std::uniform_real_distribution<double> ur( cs.meniscus, cs.bottom );
        for ( int b = 0; b < 12; b++ ) { double r0 = ur( rti ), a = 0.03 * ( b % 2 ? 1 : -1 ) * ( 0.5 + 0.5 * fabs( nd( rti ) ) );
           for ( int i = 0; i < nf; i++ ) ti[i] += a * exp( -0.5 * pow( ( xfull[i] - r0 ) / 0.0015, 2 ) );
           ti[ qBound( 0, int( ( r0 - xfull[0] ) / DR_FULL ), nf - 1 ) ] += 0.0; }
        for ( int i = 0; i < nf; i++ ) ti[i] += 0.003 * nd( rti ); }

      // object-plane signals and skewed observations (full grid), cut to fit grid
      QVector<double> obs[2][2];   // [skew][withTI]
      double maxshift = 0; int nonmono = 0; double maxgrad = 0;
      QVector<double> shiftmax_scan( ns, 0.0 );
      for ( int sk = 0; sk < 2; sk++ ) for ( int wt = 0; wt < 2; wt++ ) {
         obs[sk][wt].resize( ns * np );
         for ( int s = 0; s < ns; s++ ) {
            const double* c = ctrue.constData() + s * nf;
            QVector<double> u( nf );
            for ( int i = 0; i < nf; i++ ) u[i] = c[i] + ( wt ? ti[i] : 0.0 );
            QVector<double> o = u;
            if ( sk ) { double ms = 0; o = skew_scan( xfull, c, u.constData(), cs.kappa, ms, nonmono, cs.x0, cs.x1 );
                        if ( wt ) { shiftmax_scan[s] = ms; maxshift = qMax( maxshift, ms ); } }
            for ( int p = 0; p < np; p++ ) obs[sk][wt][ s * np + p ] = o[ fidx[p] ];
            if ( sk && wt ) for ( int p = 1; p < np; p++ )
               maxgrad = qMax( maxgrad, fabs( obs[0][0][ s*np+p ] - obs[0][0][ s*np+p-1 ] ) / ( xfit[p] - xfit[p-1] ) );
         }
      }
      fprintf( stderr, "max shift %.2e cm, max slope %.1f fringe/cm, nonmonotonic %d\n", maxshift, maxgrad, nonmono );

      // RI noise per scan
      QVector<double> ri( ns ); for ( auto& v : ri ) v = 0.3 * nd( rti );

      if ( getenv( "WCORR" ) ) {
         // ------------------------------------------------------------------
         // C1 prototype: fixed-point Wiener-skew correction with known kappa.
         // Basis simulated on an extended radial range (sources for skewed
         // points), skew operator rebuilt from the current fitted total signal,
         // applied to every basis column, 2DSA refit; TI/RI by mean removal.
         // ------------------------------------------------------------------
         const int NIT = getenv( "WNIT" ) ? atoi( getenv( "WNIT" ) ) : 6;
         QVector<double> xe; QVector<int> fsub;
         for ( int i = 0; i < nf; i += DECIM ) if ( xfull[i] <= cs.x1 + 0.05 ) {
            if ( xfull[i] >= cs.x0 && xfull[i] <= cs.x1 ) fsub << xe.size();
            xe << xfull[i]; }
         int npe = xe.size();
         QVector<US_Solute> grid;
         for ( int is = 0; is < NGS; is++ ) for ( int ik = 0; ik < NGK; ik++ )
            grid << US_Solute( cs.smin + ( cs.smax - cs.smin ) * is / ( NGS - 1 ), KMIN + ( KMAX - KMIN ) * ik / ( NGK - 1 ), 0.0 );
         auto gidx = [&]( const US_Solute& so ) {
            int is = qRound( ( so.s - cs.smin ) / ( cs.smax - cs.smin ) * ( NGS - 1 ) );
            int ik = qRound( ( so.k - KMIN ) / ( KMAX - KMIN ) * ( NGK - 1 ) );
            return is * NGK + ik; };
         QElapsedTimer et; et.start();
         QVector<QVector<float>> Ae( grid.size() );
         for ( int c0 = 0; c0 < grid.size(); c0 += 200 ) {
            QVector<US_Solute> chunk = grid.mid( c0, 200 ); QVector<QVector<double>> tmp;
            simulate_many( cs, xe, SIMPTS_FIT, tmst, chunk, tmp );
            for ( int k = 0; k < tmp.size(); k++ ) { Ae[ c0 + k ].resize( tmp[k].size() );
               for ( int i = 0; i < tmp[k].size(); i++ ) Ae[ c0 + k ][i] = (float) tmp[k][i]; }
         }
         fprintf( stderr, "extended basis %d sims (%d pts) in %.1f s\n", grid.size(), npe, et.elapsed() / 1000.0 );

         // skew operator: per (scan, fit point) source index j and weight w
         QVector<int> sj( ns * np ); QVector<double> sw( ns * np );
         QVector<QVector<double>> A( grid.size(), QVector<double>( ns * np ) );
         auto build = [&]( const QVector<US_Solute>& sol, double kap, double& maxdr ) {
            maxdr = 0;
            QVector<double> cf( npe ), rho( npe );
            for ( int s = 0; s < ns; s++ ) {
               cf.fill( 0.0 );
               for ( auto& so : sol ) { const float* a = Ae[ gidx( so ) ].constData() + s * npe;
                  for ( int i = 0; i < npe; i++ ) cf[i] += so.c * a[i]; }
               for ( int i = 0; i < npe; i++ ) {
                  int i0 = qMax( 0, i - 1 ), i1 = qMin( npe - 1, i + 1 );
                  double dr = kap * ( cf[i1] - cf[i0] ) / ( xe[i1] - xe[i0] );
                  rho[i] = xe[i] + dr;
                  if ( i > 0 && rho[i] < rho[i-1] ) rho[i] = rho[i-1] + 1e-9;   // enforce monotonic map
                  if ( xe[i] >= cs.x0 && xe[i] <= cs.x1 ) maxdr = qMax( maxdr, fabs( dr ) );
               }
               int j = 0;
               for ( int p = 0; p < np; p++ ) {
                  double x = xe[ fsub[p] ];
                  while ( j < npe - 2 && rho[j+1] < x ) j++;
                  while ( j > 0 && rho[j] > x ) j--;
                  double w = ( x <= rho[0] ) ? 0.0 : ( x >= rho[npe-1] ? 1.0 : ( x - rho[j] ) / ( rho[j+1] - rho[j] ) );
                  if ( x >= rho[npe-1] ) j = npe - 2;
                  sj[ s*np+p ] = j; sw[ s*np+p ] = qBound( 0.0, w, 1.0 );
               }
            }
            std::atomic<int> next{0};
            auto work = [&]() { for ( int c; ( c = next++ ) < grid.size(); ) {
               const float* a = Ae[c].constData(); double* o = A[c].data();
               for ( int s = 0; s < ns; s++ ) for ( int p = 0; p < np; p++ ) {
                  int k = s*np+p, j = sj[k]; double w = sw[k];
                  o[k] = ( 1.0 - w ) * a[ s*npe + j ] + w * a[ s*npe + j + 1 ]; } } };
            std::vector<std::thread> th; for ( int t = 0; t < 4; t++ ) th.emplace_back( work ); for ( auto& t : th ) t.join();
         };

         QJsonObject cj;
         cj["id"] = cs.id; cj["desc"] = cs.desc; cj["kappa"] = cs.kappa; cj["ctot"] = cs.ctot; cj["ns"] = ns; cj["np"] = np;
         QJsonArray tj; for ( int k = 0; k < tsol.size(); k++ ) { QJsonObject o; o["name"] = cs.sp[k].name;
            o["s"] = tsol[k].s; o["k"] = tsol[k].k; o["c"] = tsol[k].c; o["mw"] = cs.sp[k].mw; tj << o; }
         cj["truth"] = tj;
         QJsonArray xs; for ( double v : xfit ) xs << v; cj["x"] = xs;
         QJsonArray fits;
         struct CC { QString id; double rnd; bool tiri; int flag; int seed; };
         QVector<CC> cc = { { "none", 0, false, 0, 0 }, { "TIRI_only", 0, true, 3, 0 }, { "r0.005", 0.005, true, 3, 0 },
                            { "r0.01", 0.01, true, 3, 0 }, { "r0.01", 0.01, true, 3, 1 }, { "r0.01", 0.01, true, 3, 2 },
                            { "r0.02", 0.02, true, 3, 0 } };
         for ( auto& c : cc ) {
            // same noise realisation as the main study (seed scheme identical)
            std::mt19937 rg( 1000 + c.seed );
            QVector<double> b = obs[1][ c.tiri ? 1 : 0 ];
            for ( int s = 0; s < ns; s++ ) for ( int p = 0; p < np; p++ ) {
               double& v = b[ s * np + p ];
               if ( c.tiri ) v += ri[s];
               if ( c.rnd > 0 ) v += c.rnd * nd( rg );
            }
            QVector<US_Solute> sol;
            for ( int it = 0; it <= NIT; it++ ) {
               double maxdr = 0;
               build( sol, it == 0 ? 0.0 : cs.kappa, maxdr );
               et.restart();
               FitResult fr = fit_2dsa( A, grid, b, ns, np, c.flag );
               sol = fr.sol;
               fprintf( stderr, "  %-9s seed %d iter %d : rmsd %.5f nsol %d maxdr %.1f um (%.1f s)\n", qPrintable( c.id ), c.seed, it,
                        fr.rmsd, fr.sol.size(), maxdr * 1e4, et.elapsed() / 1000.0 );
               QJsonObject o; o["cond"] = c.id; o["rnd"] = c.rnd; o["seed"] = c.seed; o["iter"] = it; o["skew"] = 1;
               o["rmsd"] = fr.rmsd; o["maxdr"] = maxdr;
               QJsonArray sa; for ( auto& so : fr.sol ) sa << QJsonArray{ so.s, so.k, so.c }; o["solutes"] = sa;
               fits << o;
               if ( c.seed == 0 && ( it == NIT ) ) {
                  QFile f( outdir + "/" + cs.id + "_" + c.id + "_corr_resid.bin" );
                  f.open( QIODevice::WriteOnly ); f.write( (const char*) fr.resid.constData(), fr.resid.size() * 8 );
               }
            }
         }
         cj["fits"] = fits;
         QFile jf( outdir + "/" + cs.id + "_corr.json" ); jf.open( QIODevice::WriteOnly );
         jf.write( QJsonDocument( cj ).toJson( QJsonDocument::Compact ) );
         continue;
      }

      // 2DSA basis on fit grid (UltraScan default simpoints)
      QVector<US_Solute> grid;
      for ( int is = 0; is < NGS; is++ ) for ( int ik = 0; ik < NGK; ik++ )
         grid << US_Solute( cs.smin + ( cs.smax - cs.smin ) * is / ( NGS - 1 ), KMIN + ( KMAX - KMIN ) * ik / ( NGK - 1 ), 0.0 );
      QVector<QVector<double>> A;
      if ( getenv( "WDEBUG" ) ) {   // direct check: basis = true solutes only
         QVector<QVector<double>> Bt; simulate_many( cs, xfit, SIMPTS_FIT, tmst, tsol, Bt );
         QVector<int> cols; for ( int i = 0; i < tsol.size(); i++ ) cols << i;
         FitResult f = nnls_fit( Bt, cols, obs[0][0], NSCANS, np, 0, tsol );
         fprintf( stderr, "true-solute fit rmsd %.5f\n", f.rmsd );
         for ( auto& so : f.sol ) fprintf( stderr, "  s %.3f k %.3f c %.4f\n", so.s, so.k, so.c );
      }
      QElapsedTimer et; et.start();
      simulate_many( cs, xfit, SIMPTS_FIT, tmst, grid, A );
      fprintf( stderr, "basis %d sims in %.1f s\n", grid.size(), et.elapsed() / 1000.0 );
      if ( getenv( "WDEBUG" ) ) {
         QVector<int> nb, all;
         for ( int c = 0; c < grid.size(); c++ ) { all << c;
            if ( fabs( grid[c].s - tsol[0].s ) < 0.25 && fabs( grid[c].k - tsol[0].k ) < 0.21 ) nb << c; }
         FitResult f1 = nnls_fit( A, nb, obs[0][0], ns, np, 0, grid );
         fprintf( stderr, "neighbors %d: rmsd %.5f\n", nb.size(), f1.rmsd );
         for ( int c : nb ) { double s1 = 0, s2 = 0, s3 = 0; for ( int i = 0; i < ns*np; i++ ) { s1 += A[c][i]; s2 += obs[0][0][i]; }
            fprintf( stderr, "  col s %.3f k %.3f sum %.2f  datasum/ctot %.2f  A[0..3] %.3f %.3f %.3f  A[last] %.3f\n", grid[c].s, grid[c].k, s1, s2/cs.ctot, A[c][0], A[c][np/2], A[c][np*30+np/2], A[c][ns*np-1] ); }
         for ( auto& so : f1.sol ) fprintf( stderr, "  s %.3f k %.3f c %.4f\n", so.s, so.k, so.c );
         QVector<int> sub; for ( int c = 0; c < grid.size(); c += 3 ) sub << c;
         et.restart(); FitResult f2 = nnls_fit( A, sub, obs[0][0], ns, np, 0, grid );
         fprintf( stderr, "every3rd %d: rmsd %.5f nsol %d (%.1fs)\n", sub.size(), f2.rmsd, f2.sol.size(), et.elapsed()/1000. );
         return 0;
      }

      // write case info
      QJsonObject cj;
      cj["id"] = cs.id; cj["desc"] = cs.desc; cj["kappa"] = cs.kappa; cj["ctot"] = cs.ctot;
      cj["rpm"] = cs.rpm; cj["maxshift_cm"] = maxshift; cj["maxslope"] = maxgrad; cj["nonmono"] = nonmono;
      QJsonArray tj; for ( int k = 0; k < tsol.size(); k++ ) { QJsonObject o; o["name"] = cs.sp[k].name;
         o["s"] = tsol[k].s; o["k"] = tsol[k].k; o["c"] = tsol[k].c; o["mw"] = cs.sp[k].mw; tj << o; }
      cj["truth"] = tj;
      QJsonArray sh; for ( double v : shiftmax_scan ) sh << v; cj["shift_per_scan"] = sh;
      QJsonArray xs; for ( double v : xfit ) xs << v; cj["x"] = xs;
      QJsonArray ts; { Setup t; build_setup( t, cs, xfit, 200, tmst, false ); for ( auto& sc : t.ed.scanData ) ts << sc.seconds; }
      cj["t"] = ts;
      // store exemplary data: skewed minus unskewed, no noise (fit grid)
      { QFile f( outdir + "/" + cs.id + "_skewdiff.bin" ); f.open( QIODevice::WriteOnly );
        QVector<double> d( ns * np ); for ( int i = 0; i < ns * np; i++ ) d[i] = obs[1][1][i] - obs[0][1][i];
        f.write( (const char*) d.constData(), d.size() * 8 );
        QFile g( outdir + "/" + cs.id + "_data_noskew.bin" ); g.open( QIODevice::WriteOnly );
        g.write( (const char*) obs[0][0].constData(), ns * np * 8 ); }

      QJsonArray fits;
      for ( auto& nc : conds ) for ( int seed = 0; seed < nc.nseed; seed++ ) for ( int sk = 0; sk < 2; sk++ ) {
         std::mt19937 rg( 1000 + seed );
         QVector<double> b = obs[sk][ nc.tiri ? 1 : 0 ];
         for ( int s = 0; s < ns; s++ ) for ( int p = 0; p < np; p++ ) {
            double& v = b[ s * np + p ];
            if ( nc.tiri ) v += ri[s];
            if ( nc.rnd > 0 ) v += nc.rnd * nd( rg );
         }
         et.restart();
         FitResult fr = fit_2dsa( A, grid, b, ns, np, nc.fitflag );
         fprintf( stderr, "  %-10s seed %d skew %d : rmsd %.5f  nsol %d  (%.1f s)\n", qPrintable( nc.id ), seed, sk,
                  fr.rmsd, fr.sol.size(), et.elapsed() / 1000.0 );
         QJsonObject o; o["cond"] = nc.id; o["rnd"] = nc.rnd; o["seed"] = seed; o["skew"] = sk; o["rmsd"] = fr.rmsd;
         QJsonArray sj; for ( auto& so : fr.sol ) sj << QJsonArray{ so.s, so.k, so.c };
         o["solutes"] = sj;
         if ( nc.tiri ) {   // fitted TI vs true (object-plane) TI, mean-removed
            QJsonArray a; for ( double v : fr.ti ) a << v; o["ti_fit"] = a;
         }
         fits << o;
         if ( seed == 0 ) {   // residual map
            QFile f( outdir + "/" + cs.id + "_" + nc.id + "_sk" + QString::number( sk ) + "_resid.bin" );
            f.open( QIODevice::WriteOnly ); f.write( (const char*) fr.resid.constData(), fr.resid.size() * 8 );
         }
      }
      QJsonArray tij; for ( int p = 0; p < np; p++ ) tij << ti[ fidx[p] ]; cj["ti_true"] = tij;
      cj["fits"] = fits; cj["ns"] = ns; cj["np"] = np;
      QFile jf( outdir + "/" + cs.id + ".json" ); jf.open( QIODevice::WriteOnly );
      jf.write( QJsonDocument( cj ).toJson( QJsonDocument::Compact ) );
   }
   return 0;
}
