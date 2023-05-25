#include <QPrinter>
#include <QPdfWriter>
#include <QPainter>

#include "us_esigner_gmp.h"
#include "us_settings.h"
#include "us_gui_settings.h"
#include "us_gui_util.h"
#include "us_protocol_util.h"
#include "us_run_protocol.h"
#include "us_math2.h"
#include "us_constants.h"
#include "us_solution_vals.h"
#include "us_tar.h"

#define MIN_NTC 25

// Constructor
US_eSignaturesGMP::US_eSignaturesGMP() : US_Widgets()
{
  auto_mode  = false;
  
  setWindowTitle( tr( "GMP e-Signatures"));
  //setPalette( US_GuiSettings::frameColor() );
  setPalette( US_GuiSettings::normalColor() );

  //main V-layout
  QVBoxLayout* mainLayout     = new QVBoxLayout( this );
  mainLayout->setSpacing        ( 2 );
  mainLayout->setContentsMargins( 2, 2, 2, 2 );

  //for setting global reviewers: people, permits
  QGridLayout* revGlobalGrid  = new QGridLayout();
  revGlobalGrid->setSpacing     ( 2 );
  revGlobalGrid->setContentsMargins( 2, 2, 2, 2 );

  QLabel* bn_revGlobal     = us_banner( tr( "Assign Global Reviewers from the List of Investigators:" ), 1 );
  QFontMetrics m (bn_revGlobal -> font()) ;
  int RowHeight = m.lineSpacing() ;
  bn_revGlobal -> setFixedHeight  (1.5 * RowHeight);

  lb_inv_search     = us_label( tr( "Investigator Search:" ) );
  le_inv_search     = us_lineedit();
  lw_inv_list       = us_listwidget();
  lw_inv_list       ->setSelectionMode( QAbstractItemView::ExtendedSelection );
  
  QLabel* lb_inv_smry     = us_label( tr( "Investigator Information:" ), 1 );
  te_inv_smry      = us_textedit();
  te_inv_smry      ->setTextColor( Qt::blue );
  te_inv_smry      ->setFont( QFont( US_Widgets::fixedFont().family(),
                                    US_GuiSettings::fontSize() - 1) );
  us_setReadOnly( te_inv_smry, true );

  lb_grev_search     = us_label( tr( "Global Reviewer Search:" ) );
  le_grev_search     = us_lineedit();
  lw_grev_list       = us_listwidget();
  lw_grev_list       ->setSelectionMode( QAbstractItemView::ExtendedSelection );
  
  QLabel* lb_grev_smry     = us_label( tr( "Reviewer Information:" ), 1 );
  te_grev_smry      = us_textedit();
  te_grev_smry      ->setTextColor( Qt::blue );
  te_grev_smry      ->setFont( QFont( US_Widgets::fixedFont().family(),
                                    US_GuiSettings::fontSize() - 1) );
  us_setReadOnly( te_grev_smry, true );

  pb_set_global_rev    = us_pushbutton( tr( "Set Investigator as Global Reviewer" ) );
  pb_unset_global_rev  = us_pushbutton( tr( "Remove from List of Global Reviewers" ) );

  pb_set_global_rev   ->setEnabled( false );
  pb_unset_global_rev ->setEnabled( false );

  int row = 0;
  revGlobalGrid -> addWidget( bn_revGlobal,             row++,    0, 1,  20 );
  
  revGlobalGrid -> addWidget( lb_inv_search,   row,      0, 1,  2  );
  revGlobalGrid -> addWidget( le_inv_search,   row,      2, 1,  3  );

  revGlobalGrid -> addWidget( lb_inv_smry,     row,      5, 1,  5  );
  
  revGlobalGrid -> addWidget( lb_grev_search,  row,      10, 1, 2  );
  revGlobalGrid -> addWidget( le_grev_search,  row,      12, 1, 3  );

  revGlobalGrid -> addWidget( lb_grev_smry,    row++,    15, 1, 5  );
  
  revGlobalGrid -> addWidget( lw_inv_list,     row,      0,  3,  5  );
  revGlobalGrid -> addWidget( te_inv_smry,     row,      5,  3,  5  );
  revGlobalGrid -> addWidget( lw_grev_list,    row,      10, 3,  5  );
  revGlobalGrid -> addWidget( te_grev_smry,    row++,    15, 3,  5  );

  row += 5;
  revGlobalGrid -> addWidget( pb_set_global_rev,       row,       0,  1,  10  );
  revGlobalGrid -> addWidget( pb_unset_global_rev,     row++,     10, 1,  10  );

  connect( le_inv_search, SIGNAL( textChanged( const QString& ) ), 
	   SLOT  ( limit_inv_names( const QString& ) ) );
  connect( lw_inv_list, SIGNAL( itemClicked( QListWidgetItem* ) ), 
	   SLOT  ( get_inv_data  ( QListWidgetItem* ) ) );

  connect( le_grev_search, SIGNAL( textChanged( const QString& ) ), 
	   SLOT  ( limit_grev_names( const QString& ) ) );
  connect( lw_grev_list, SIGNAL( itemClicked( QListWidgetItem* ) ), 
	   SLOT  ( get_grev_data  ( QListWidgetItem* ) ) );

  connect( pb_set_global_rev,   SIGNAL( clicked() ), SLOT ( set_greviewer() ) );
  connect( pb_unset_global_rev, SIGNAL( clicked() ), SLOT ( unset_greviewer() ) );
 
  //for setting oper, revs. for selected GMP Run
  QGridLayout*  revOperGMPRunGrid  = new QGridLayout();
  revOperGMPRunGrid->setSpacing     ( 2 );
  revOperGMPRunGrid->setContentsMargins( 1, 1, 1, 1 );

  QLabel* bn_revOperGMP     = us_banner( tr( "Assign Operator and Reviewer(s) for GMP Run:" ), 1 );
  bn_revOperGMP -> setFixedHeight  (1.5 * RowHeight);

  pb_selRun_operRev_set = us_pushbutton( tr( "Select GMP Run" ) );
  pb_set_operRev        = us_pushbutton( tr( "Assign/Change Operators and Reviewers" ) );
  pb_set_operRev -> setEnabled( false );

  pb_add_oper      = us_pushbutton( tr( "Add" ) );
  pb_add_oper       -> setEnabled( false );
  pb_remove_oper   = us_pushbutton( tr( "Remove Last" ) );
  pb_remove_oper   -> setEnabled( false );
  pb_add_rev       = us_pushbutton( tr( "Add" ) );
  pb_remove_rev    = us_pushbutton( tr( "Remove Last" ) );
  
  QLabel* lb_run_name       = us_label( "Run Name:" );
  QLabel* lb_optima_name    = us_label( "Optima:" );
  QLabel* lb_operator_names = us_label( "Assigned Operators:" );
  QLabel* lb_reviewer_names = us_label( "Assigned Reviewers:" );
  
  QLabel* lb_choose_oper      = us_label( "Choose Operator:" );
  QLabel* lb_choose_rev       = us_label( "Choose Reviewer:" );
  QLabel* lb_opers_to_assign  = us_label( "Operators to Assign:" );
  QLabel* lb_revs_to_assign   = us_label( "Reviewers to Assign:" );
  
  le_run_name       = us_lineedit( tr(""), 0, true );
  le_optima_name    = us_lineedit( tr(""), 0, true );


  te_operator_names    = us_textedit();
  //te_operator_names    ->setTextColor( Qt::blue );
  te_operator_names    -> setFixedHeight  ( RowHeight * 2 );
  te_operator_names    ->setFont( QFont( US_Widgets::fixedFont().family(),
					 US_GuiSettings::fontSize() - 1) );
  us_setReadOnly( te_operator_names, true );

  te_reviewer_names    = us_textedit();
  //te_reviewer_names    ->setTextColor( Qt::blue );
  te_reviewer_names    -> setFixedHeight  ( RowHeight * 3 );
  te_reviewer_names    ->setFont( QFont( US_Widgets::fixedFont().family(),
					 US_GuiSettings::fontSize() - 1) );
  us_setReadOnly( te_reviewer_names, true );


  te_opers_to_assign    = us_textedit();
  //te_opers_to_assign    ->setTextColor( Qt::blue );
  te_opers_to_assign    -> setFixedHeight  ( RowHeight * 2 );
  te_opers_to_assign    ->setFont( QFont( US_Widgets::fixedFont().family(),
					 US_GuiSettings::fontSize() - 1) );
  us_setReadOnly( te_opers_to_assign, true );

  te_revs_to_assign    = us_textedit();
  //te_revs_to_assign    ->setTextColor( Qt::blue );
  te_revs_to_assign    -> setFixedHeight  ( RowHeight * 3 );
  te_revs_to_assign    ->setFont( QFont( US_Widgets::fixedFont().family(),
					 US_GuiSettings::fontSize() - 1) );
  us_setReadOnly( te_revs_to_assign, true );
  
  
  cb_choose_operator   = new QComboBox( this );
  cb_choose_rev        = new QComboBox( this );
   
  row = 0;
  revOperGMPRunGrid -> addWidget( bn_revOperGMP,          row++,    0,  1,  14 );
  
  revOperGMPRunGrid -> addWidget( pb_selRun_operRev_set,  row++,    0,  1,  6 );

  revOperGMPRunGrid -> addWidget( lb_run_name,            row,      0,  1,  2 );
  revOperGMPRunGrid -> addWidget( le_run_name,            row,      2,  1,  4 );
  revOperGMPRunGrid -> addWidget( lb_choose_oper,         row,      7,  1,  2 );
  revOperGMPRunGrid -> addWidget( cb_choose_operator,     row,      9,  1,  3 );
  revOperGMPRunGrid -> addWidget( pb_add_oper,            row++,    12, 1,  2 );

  revOperGMPRunGrid -> addWidget( lb_optima_name,         row,      0,  1,  2 );
  revOperGMPRunGrid -> addWidget( le_optima_name,         row,      2,  1,  4 );
  revOperGMPRunGrid -> addWidget( lb_choose_rev,          row,      7,  1,  2 );
  revOperGMPRunGrid -> addWidget( cb_choose_rev,          row,      9,  1,  3 );
  revOperGMPRunGrid -> addWidget( pb_add_rev,             row++,    12, 1,  2 );
  
  revOperGMPRunGrid -> addWidget( lb_operator_names,      row,      0, 1,  2 );
  revOperGMPRunGrid -> addWidget( te_operator_names,      row,      2, 1,  4 );
  revOperGMPRunGrid -> addWidget( lb_opers_to_assign,     row,      7,  1,  2 );
  revOperGMPRunGrid -> addWidget( te_opers_to_assign,     row,      9,  1,  3 );
  revOperGMPRunGrid -> addWidget( pb_remove_oper,         row++,    12, 1,  2 );

  revOperGMPRunGrid -> addWidget( lb_reviewer_names,      row,      0,  1,  2 );
  revOperGMPRunGrid -> addWidget( te_reviewer_names,      row,      2,  1,  4 );
  revOperGMPRunGrid -> addWidget( lb_revs_to_assign,      row,      7,  1,  2 );
  revOperGMPRunGrid -> addWidget( te_revs_to_assign,      row,      9,  1,  3 );
  revOperGMPRunGrid -> addWidget( pb_remove_rev,          row++,    12, 1,  2 );

  revOperGMPRunGrid -> addWidget( pb_set_operRev,         row++,    7,  1,  6 );

  connect( pb_selRun_operRev_set,   SIGNAL( clicked() ), SLOT ( selectGMPRun() ) );
  connect( pb_set_operRev, SIGNAL( clicked() ), SLOT ( assignOperRevs() ) );
  connect( pb_add_oper, SIGNAL( clicked() ), SLOT ( addOpertoList() ) );
  connect( pb_remove_oper, SIGNAL( clicked() ), SLOT ( removeOperfromList() ) );
  connect( pb_add_rev, SIGNAL( clicked() ), SLOT ( addRevtoList() ) );
  connect( pb_remove_rev, SIGNAL( clicked() ), SLOT ( removeRevfromList() ) );
  
  //for eSigning selected GMP Run
  QGridLayout* eSignGMPRunGrid     = new QGridLayout();
  eSignGMPRunGrid->setSpacing        ( 2 );
  eSignGMPRunGrid->setContentsMargins( 1, 1, 1, 1 );

  QLabel* bn_eSignGMP     = us_banner( tr( "Manage e-Signatures for GMP Run:" ), 1 );
  bn_eSignGMP -> setFixedHeight  (1.5 * RowHeight);

  pb_loadreport_db          =  us_pushbutton( tr( "Load GMP Report from DB (.PDF)" ) );
  pb_view_report_db         =  us_pushbutton( tr( "Review Downloaded Report" ) );
  pb_esign_report           =  us_pushbutton( tr( "e-Sign Report" ) );

  pb_view_report_db  -> setEnabled( false );
  pb_esign_report    -> setEnabled( false );

  pb_esign_report    -> setStyleSheet( tr("QPushButton::enabled {background: #556B2F; color: lightgray; }"
					  "QPushButton::disabled {background: #90EE90; color: black}" ));

  QLabel* lb_loaded_run_db  = us_label( tr( "Loaded GMP Report for Run:" ) );
  le_loaded_run_db          = us_lineedit( tr(""), 0, true );
  
  //Filename path
  QLabel*      lb_fpath_info = us_label( tr( "Report File \nLocation:" ) );
  te_fpath_info =  us_textedit();
  te_fpath_info -> setFixedHeight  ( RowHeight * 3 );
  te_fpath_info -> setText( tr( "" ) );
  us_setReadOnly( te_fpath_info, true );
  
  row = 0;
  eSignGMPRunGrid -> addWidget( bn_eSignGMP,        row++,    0, 1,  14 );

  eSignGMPRunGrid -> addWidget( pb_loadreport_db,   row++,    0, 1,  6);
  
  eSignGMPRunGrid -> addWidget( lb_loaded_run_db,   row,      0, 1,  3);
  eSignGMPRunGrid -> addWidget( le_loaded_run_db,   row,      3, 1,  4);

  eSignGMPRunGrid -> addWidget( pb_view_report_db , row++,    8,  1,  4 );

  eSignGMPRunGrid -> addWidget( lb_fpath_info,      row,      0, 1,  3);
  eSignGMPRunGrid -> addWidget( te_fpath_info,      row,      3, 1,  4);

  eSignGMPRunGrid -> addWidget( pb_esign_report,    row++,    8,  1,  4 );
  

  //Setting top-level Layouts:
  mainLayout -> addLayout( revGlobalGrid );
  mainLayout -> addLayout( revOperGMPRunGrid );
  mainLayout -> addLayout( eSignGMPRunGrid );

  mainLayout -> addStretch();

  //initialize investigators, global reviewers
  init_invs();
  init_grevs();
			 
  resize( 1200, 700 );
}


//For autoflow: constructor
US_eSignaturesGMP::US_eSignaturesGMP( QString a_mode ) : US_Widgets()
{
  auto_mode  = true;

  setWindowTitle( tr( "GMP e-Signatures"));
  setPalette( US_GuiSettings::frameColor() );

  // primary layouts
  QHBoxLayout* mainLayout     = new QHBoxLayout( this );
  mainLayout->setSpacing        ( 2 );
  mainLayout->setContentsMargins( 2, 2, 2, 2 );

  resize( 1000, 700 );
}


//For the end of 1. EXP: defined by admin
US_eSignaturesGMP::US_eSignaturesGMP( QMap< QString, QString > & protocol_details ) : US_Widgets()
{
  this->protocol_details = protocol_details;
  
  setWindowTitle( tr( "GMP e-Signatures"));
  setPalette( US_GuiSettings::frameColor() );

  // primary layouts
  QHBoxLayout* mainLayout     = new QHBoxLayout( this );
  mainLayout->setSpacing        ( 2 );
  mainLayout->setContentsMargins( 2, 2, 2, 2 );

  resize( 1000, 700 );
}


//init investigators list
void US_eSignaturesGMP::init_invs( void )
{
  investigators. clear();
  
  US_Passwd   pw;
  QString     masterPW  = pw.getPasswd();
  US_DB2      db( masterPW );  // New constructor

  if ( db.lastErrno() != US_DB2::OK )
    {
      // Error message here
      QMessageBox::information( this,
				tr( "DB Connection Problem" ),
				tr( "There was an error connecting to the database:\n" ) 
				+ db.lastError() );
      return;
    }

  lw_inv_list-> clear();

  QStringList query;
  query << "get_people_inv" << "%" + le_inv_search->text() + "%";
  qDebug() << "init_invs(), query --  " << query;
  db.query( query );

  US_InvestigatorData data;
  int inv = US_Settings::us_inv_ID();
  int lev = US_Settings::us_inv_level();
  
  while ( db.next() )
    {
      data.invID     = db.value( 0 ).toInt();
      data.lastName  = db.value( 1 ).toString();
      data.firstName = db.value( 2 ).toString();
            
      if ( lev < 3  &&  inv != data.invID )
	continue;
      
      investigators << data;
      
      lw_inv_list-> addItem( new QListWidgetItem( 
						 "InvID: (" + QString::number( data.invID ) + "), " + 
						 data.lastName + ", " + data.firstName ) );
    }
}


void US_eSignaturesGMP::limit_inv_names( const QString& s )
{
  
  lw_inv_list->clear();
  
  for ( int i = 0; i < investigators.size(); i++ )
   {
     if ( investigators[ i ].lastName.contains( 
					       QRegExp( ".*" + s + ".*", Qt::CaseInsensitive ) ) ||
	  investigators[ i ].firstName.contains(
						QRegExp( ".*" + s + ".*", Qt::CaseInsensitive ) ) )
       lw_inv_list->addItem( new QListWidgetItem(
						 "InvID: (" + QString::number( investigators[ i ].invID ) + "), " +
						 investigators[ i ].lastName + ", " + 
						 investigators[ i ].firstName ) );
   }
}

void US_eSignaturesGMP::limit_grev_names( const QString& s )
{
  lw_grev_list->clear();
  
  for ( int i = 0; i < g_reviewers.size(); i++ )
   {
     if ( g_reviewers[ i ].lastName.contains( 
					       QRegExp( ".*" + s + ".*", Qt::CaseInsensitive ) ) ||
	  g_reviewers[ i ].firstName.contains(
						QRegExp( ".*" + s + ".*", Qt::CaseInsensitive ) ) )
       lw_grev_list->addItem( new QListWidgetItem(
						 "InvID: (" + QString::number( g_reviewers[ i ].invID ) + "), " +
						 g_reviewers[ i ].lastName + ", " + 
						 g_reviewers[ i ].firstName ) );
   }
}


void US_eSignaturesGMP::get_inv_data( QListWidgetItem* item )
{
   QString entry = item->text();
   
   int     left  = entry.indexOf( '(' ) + 1;
   int     right = entry.indexOf( ')' );
   QString invID = entry.mid( left, right - left );

   US_Passwd   pw;
   QString     masterPW  = pw.getPasswd();
   US_DB2      db( masterPW ); 

   if ( db.lastErrno() != US_DB2::OK )
   {
      QMessageBox::information( this,
         tr( "DB Connection Problem" ),
         tr( "There was an error connecting to the database:\n" ) 
             + db.lastError() );
      return;
   } 

   QStringList query;
   query << "get_person_info" << invID; 
      
   db.query( query );
   db.next();

   info.invID        = invID.toInt();
   info.firstName    = db.value( 0 ).toString();
   info.lastName     = db.value( 1 ).toString();
   info.address      = db.value( 2 ).toString();
   info.city         = db.value( 3 ).toString();
   info.state        = db.value( 4 ).toString();
   info.zip          = db.value( 5 ).toString();
   info.phone        = db.value( 6 ).toString();
   info.organization = db.value( 7 ).toString();
   info.email        = db.value( 8 ).toString();
   info.invGuid      = db.value( 9 ).toString();
   info.ulev         = db.value( 10 ).toInt();
   info.gmpReviewer  = db.value( 11 ).toInt();

   te_inv_smry->setText( get_inv_or_grev_smry( info, "Investigator") );


   pb_set_global_rev -> setEnabled( true );
}

// Message string for investigator summary
QString US_eSignaturesGMP::get_inv_or_grev_smry( US_InvestigatorData p_info, QString p_type )
{
  QString smry;
  QStringList mlines;

  mlines << QString( tr( "== Selected %1 ==\n" )). arg( p_type );
    
  mlines << "Last Name:\n "       +  p_info.lastName; 
  mlines << "First Name:\n "      +  p_info.firstName;
  mlines << "User Level:\n "      +  QString::number( p_info.ulev ) ;
  QString grev_set = (p_info.gmpReviewer) ? "YES" : "NO" ;
  mlines << "GMP Reviewer:\n "    +  grev_set;
  mlines << "Email:\n "           +  p_info.email       ; 
  mlines << "Organization:\n "    +  p_info.organization; 

  // mlines << "Investigator ID:\n\t" +  p_info.invID ;
  // mlines << "Address:\t"         +  p_info.address     ;
  // mlines << "City:\t"            +  p_info.city        ; 
  // mlines << "State:\t"           +  p_info.state       ; 
  // mlines << "Zip:\t"             +  p_info.zip         ; 
  // mlines << "Phone:\t"           +  p_info.phone       ; 
  
  for ( int ii = 0; ii < mlines.count(); ii++ )
    smry         += mlines[ ii ] + "\n";
   

  return smry;
}


//init global reviewers list
void US_eSignaturesGMP::init_grevs( void )
{
  g_reviewers. clear();
  lw_grev_list   -> clear();
  cb_choose_rev  -> clear();
  
  US_Passwd   pw;
  QString     masterPW  = pw.getPasswd();
  US_DB2      db( masterPW );  // New constructor

  if ( db.lastErrno() != US_DB2::OK )
    {
      // Error message here
      QMessageBox::information( this,
				tr( "DB Connection Problem" ),
				tr( "There was an error connecting to the database:\n" ) 
				+ db.lastError() );
      return;
    }
  
  QStringList query;
  query << "get_people_grev" << "%" + le_grev_search->text() + "%";
  qDebug() << "init_invs(), query --  " << query;
  db.query( query );

  US_InvestigatorData data;
  int inv = US_Settings::us_inv_ID();
  int lev = US_Settings::us_inv_level();
  
  while ( db.next() )
    {
      data.invID     = db.value( 0 ).toInt();
      data.lastName  = db.value( 1 ).toString();
      data.firstName = db.value( 2 ).toString();
            
      if ( lev < 3  &&  inv != data.invID )
	continue;

      g_reviewers << data;
      //populate lists
      lw_grev_list-> addItem( new QListWidgetItem( 
						  "InvID: (" + QString::number( data.invID ) + "), " + 
						  data.lastName + ", " + data.firstName ) );
      cb_choose_rev->addItem( QString::number( data.invID ) + ". " + 
			      data.lastName + ", " + data.firstName );
      
    }
}


void US_eSignaturesGMP::get_grev_data( QListWidgetItem* item )
{
   QString entry = item->text();
   
   int     left  = entry.indexOf( '(' ) + 1;
   int     right = entry.indexOf( ')' );
   QString invID = entry.mid( left, right - left );

   US_Passwd   pw;
   QString     masterPW  = pw.getPasswd();
   US_DB2      db( masterPW ); 

   if ( db.lastErrno() != US_DB2::OK )
   {
      QMessageBox::information( this,
         tr( "DB Connection Problem" ),
         tr( "There was an error connecting to the database:\n" ) 
             + db.lastError() );
      return;
   } 

   QStringList query;
   query << "get_person_info" << invID; 
      
   db.query( query );
   db.next();

   info_grev.invID        = invID.toInt();
   info_grev.firstName    = db.value( 0 ).toString();
   info_grev.lastName     = db.value( 1 ).toString();
   info_grev.address      = db.value( 2 ).toString();
   info_grev.city         = db.value( 3 ).toString();
   info_grev.state        = db.value( 4 ).toString();
   info_grev.zip          = db.value( 5 ).toString();
   info_grev.phone        = db.value( 6 ).toString();
   info_grev.organization = db.value( 7 ).toString();
   info_grev.email        = db.value( 8 ).toString();
   info_grev.invGuid      = db.value( 9 ).toString();
   info_grev.ulev         = db.value( 10 ).toInt();
   info_grev.gmpReviewer  = db.value( 11 ).toInt();

   te_grev_smry->setText( get_inv_or_grev_smry( info_grev, "Reviewer") );

   pb_unset_global_rev -> setEnabled( true );
}

void US_eSignaturesGMP::set_greviewer()
{

  QString entry = lw_inv_list->currentItem()->text();
  qDebug() << "Set gRev: -- " << entry;

  int     left  = entry.indexOf( '(' ) + 1;
  int     right = entry.indexOf( ')' );
  QString invID = entry.mid( left, right - left );

  US_Passwd   pw;
  QString     masterPW  = pw.getPasswd();
  US_DB2      db( masterPW ); 
  
   if ( db.lastErrno() != US_DB2::OK )
     {
       QMessageBox::information( this,
				 tr( "DB Connection Problem" ),
				 tr( "There was an error connecting to the database:\n" ) 
				 + db.lastError() );
       return;
     }
   
   QStringList query;
   query << "set_person_grev_status" << invID;

   db.query( query );
   db.next();

   //update both inv && grev lw_lists
   lw_inv_list -> clear();
   lw_grev_list-> clear();

   le_inv_search   ->setText("");
   le_grev_search  ->setText("");
   
   te_inv_smry -> clear();
   te_grev_smry-> clear();
   
   init_invs();
   init_grevs();
}

void US_eSignaturesGMP::unset_greviewer()
{
  QString entry = lw_grev_list->currentItem()->text();
  qDebug() << "[UN]Set gRev: -- " << entry;
  
  int     left  = entry.indexOf( '(' ) + 1;
  int     right = entry.indexOf( ')' );
  QString invID = entry.mid( left, right - left );
  
  US_Passwd   pw;
  QString     masterPW  = pw.getPasswd();
  US_DB2      db( masterPW ); 
  
  if ( db.lastErrno() != US_DB2::OK )
    {
      QMessageBox::information( this,
				tr( "DB Connection Problem" ),
				tr( "There was an error connecting to the database:\n" ) 
				+ db.lastError() );
      return;
    }
  
   QStringList query;
   query << "unset_person_grev_status" << invID;

   db.query( query );
   db.next();
  
   //update both inv && grev lw_lists
   lw_inv_list -> clear();
   lw_grev_list-> clear();

   le_inv_search   ->setText("");
   le_grev_search  ->setText("");
   
   te_inv_smry -> clear();
   te_grev_smry-> clear();
    
   init_invs();
   init_grevs();

   ///Debug
   for(int i=0; i<investigators.size(); ++i )
     qDebug() << "inv after unsetting -- " << investigators[i].lastName;

}


void US_eSignaturesGMP::selectGMPRun( void )
{
  list_all_autoflow_records( autoflowdata  );

  QString pdtitle( tr( "Select GMP Run" ) );
  QStringList hdrs;
  int         prx;
  hdrs << "ID"
       << "Run Name"
       << "Optima Name"
       << "Created"
       << "Run Status"
       << "Stage"
       << "GMP";
  
  QString autoflow_btn = "AUTOFLOW_GMP_REPORT";

  pdiag_autoflow = new US_SelectItem( autoflowdata, hdrs, pdtitle, &prx, autoflow_btn, -2 );

  QString autoflow_id_selected("");
  if ( pdiag_autoflow->exec() == QDialog::Accepted )
    {
      autoflow_id_selected  = autoflowdata[ prx ][ 0 ];

      //reset Gui && internal structures
      reset_set_revOper_panel();
    }
  else
    return;

  // Get detailed info on the autoflow record
  int autoflowID = autoflow_id_selected.toInt();
  gmp_run_details = read_autoflow_record( autoflowID );

  set_revOper_panel_gui();

  //Enable button to change/set assigned oper(s) / rev(s) 
  pb_add_oper    -> setEnabled( true );
  pb_remove_oper -> setEnabled( true );
}


void US_eSignaturesGMP::reset_set_revOper_panel( void )
{
  le_run_name        -> clear();
  le_optima_name     -> clear();
  te_operator_names  -> clear();
  te_reviewer_names  -> clear();
  cb_choose_operator -> clear();

  te_opers_to_assign -> clear();
  te_revs_to_assign  -> clear();

  //main QMap for the loaded GMP run
  gmp_run_details   .clear();
  eSign_details     .clear();
  isEsignRecord     = false;
  
  pb_set_operRev -> setEnabled( false );
}

void US_eSignaturesGMP::set_revOper_panel_gui( void )
{
  QString full_runname = gmp_run_details[ "filename" ];
  QString FullRunName_auto = gmp_run_details[ "experimentName" ] + "-run" + gmp_run_details[ "runID" ];
  if ( full_runname.contains(",") && full_runname.contains("IP") && full_runname.contains("RI") )
    FullRunName_auto += " (combined RI+IP)";
    
  QString OptimaName = gmp_run_details["OptimaName"]; 
  le_run_name        -> setText( FullRunName_auto );
  le_optima_name     -> setText( OptimaName );

  //Set Operators for Optima:
  QStringList sl_operators = read_operators( gmp_run_details[ "OptimaID" ] );
  cb_choose_operator -> addItems( sl_operators );

  //Read autoflowGMPReportEsign record by autoflowID:
  eSign_details = read_autoflowGMPReportEsign_record( gmp_run_details[ "autoflowID" ] );
  
  //&& Set defined Operator/Reviewers (if any)
  if ( !eSign_details. contains("operatorListJson") )
    te_operator_names  -> setText( "NOT SET" );
  else
    {
      QJsonDocument jsonDocOperList = QJsonDocument::fromJson( eSign_details[ "operatorListJson" ] .toUtf8() );
      te_operator_names -> setText( get_assigned_oper_revs( jsonDocOperList ) );

      
    }
  
  if ( !eSign_details. contains("reviewersListJson") )
    te_reviewer_names  -> setText( "NOT SET" );
  else
    {
      QJsonDocument jsonDocRevList  = QJsonDocument::fromJson( eSign_details[ "reviewersListJson" ] .toUtf8() );
      te_reviewer_names  -> setText( get_assigned_oper_revs( jsonDocRevList ) );
	
    }
}

//form a string of opers/revs out of jsonDoc
QString US_eSignaturesGMP::get_assigned_oper_revs( QJsonDocument jsonDoc )
{
  QString smry;
  QStringList assigned_list;
  
  if ( !jsonDoc. isArray() )
    {
      qDebug() << "jsonDoc not a JSON, and/or not an JSON Array!";
      return smry;
    }
  
  QJsonArray jsonDoc_array  = jsonDoc.array();
  for (int i = 0; i < jsonDoc_array.size(); ++i )
    assigned_list << jsonDoc_array[i].toString();
  
  for ( int ii = 0; ii < assigned_list.count(); ii++ )
    {
      smry += assigned_list[ ii ];
      if ( ii != assigned_list.count() -1 )
	smry += "\n";
    }
  
  return smry;
}

// Query autoflow (history) table for records
int US_eSignaturesGMP::list_all_autoflow_records( QList< QStringList >& autoflowdata )
{
  int nrecs        = 0;   
  autoflowdata.clear();

  QStringList qry;
  
  US_Passwd pw;
  US_DB2* db = new US_DB2( pw.getPasswd() );
  
  if ( db->lastErrno() != US_DB2::OK )
    {
      QMessageBox::warning( this, tr( "LIMS DB Connection Problem" ),
			    tr( "Could not connect to database \n" ) + db->lastError() );

      return nrecs;
    }
  
  //Check user level && ID
  QStringList defaultDB = US_Settings::defaultDB();
  QString user_guid   = defaultDB.at( 9 );
  
  //get personID from personGUID
  qry.clear();
  qry << QString( "get_personID_from_GUID" ) << user_guid;
  db->query( qry );
  
  int user_id = 0;
  
  if ( db->next() )
    user_id = db->value( 0 ).toInt();


  //deal with autoflow descriptions
  qry. clear();
  qry << "get_autoflow_desc";
  db->query( qry );

  while ( db->next() )
    {
      QStringList autoflowentry;
      QString id                 = db->value( 0 ).toString();
      QString runname            = db->value( 5 ).toString();
      QString status             = db->value( 8 ).toString();
      QString optimaname         = db->value( 10 ).toString();
      
      QDateTime time_started     = db->value( 11 ).toDateTime().toUTC();
      QString invID              = db->value( 12 ).toString();

      QDateTime time_created     = db->value( 13 ).toDateTime().toUTC();
      QString gmpRun             = db->value( 14 ).toString();
      QString full_runname       = db->value( 15 ).toString();

      QString operatorID         = db->value( 16 ).toString();
      QString devRecord          = db->value( 18 ).toString();

      QDateTime local(QDateTime::currentDateTime());

      if ( devRecord == "Processed" )
	continue;
      
      //process runname: if combined, correct for nicer appearance
      if ( full_runname.contains(",") && full_runname.contains("IP") && full_runname.contains("RI") )
	{
	  QString full_runname_edited  = full_runname.split(",")[0];
	  full_runname_edited.chop(3);

	  full_runname = full_runname_edited + " (combined RI+IP) ";
	  runname += " (combined RI+IP) ";
	}
      
      autoflowentry << id << runname << optimaname  << time_created.toString(); // << time_started.toString(); // << local.toString( Qt::ISODate );

      if ( time_started.toString().isEmpty() )
	autoflowentry << QString( tr( "NOT STARTED" ) );
      else
	{
	  if ( status == "LIVE_UPDATE" )
	    autoflowentry << QString( tr( "RUNNING" ) );
	  if ( status == "EDITING" || status == "EDIT_DATA" || status == "ANALYSIS" || status == "REPORT" )
	    autoflowentry << QString( tr( "COMPLETED" ) );
	    //autoflowentry << time_started.toString();
	}

      if ( status == "EDITING" )
	status = "LIMS_IMPORT";
      
      autoflowentry << status << gmpRun;

      //Check user level && GUID; if <3, check if the user is operator || investigator
      if ( US_Settings::us_inv_level() < 3 )
	{
	  qDebug() << "User level low: " << US_Settings::us_inv_level();
	  qDebug() << "user_id, operatorID.toInt(), invID.toInt() -- " << user_id << operatorID.toInt() << invID.toInt();

	  //if ( user_id && ( user_id == operatorID.toInt() || user_id == invID.toInt() ) )
	  if ( user_id && user_id == invID.toInt() )
	    {//Do we allow operator as defined in autoflow record to also see reports?? 
	    
	      autoflowdata  << autoflowentry;
	      nrecs++;
	    }
	}
      else
	{
	  autoflowdata  << autoflowentry;
	  nrecs++;
	}
      
    }

  return nrecs;
}


// Query autoflow record
QMap< QString, QString>  US_eSignaturesGMP::read_autoflow_record( int autoflowID  )
{
   // Check DB connection
   US_Passwd pw;
   QString masterpw = pw.getPasswd();
   US_DB2* db = new US_DB2( masterpw );

   QMap <QString, QString> run_info;
   
   if ( db->lastErrno() != US_DB2::OK )
     {
       QMessageBox::warning( this, tr( "Connection Problem" ),
			     tr( "Read protocol: Could not connect to database \n" ) + db->lastError() );
       return run_info;
     }

   QStringList qry;
   qry << "read_autoflow_record"
       << QString::number( autoflowID );
   
   db->query( qry );

   if ( db->lastErrno() == US_DB2::OK )      // Autoflow record exists
     {
       while ( db->next() )
	 {
	   run_info[ "protocolName" ]   = db->value( 0 ).toString();
	   run_info[ "CellChNumber" ]   = db->value( 1 ).toString();
	   run_info[ "TripleNumber" ]   = db->value( 2 ).toString();
	   run_info[ "duration" ]       = db->value( 3 ).toString();
	   run_info[ "experimentName" ] = db->value( 4 ).toString();
	   run_info[ "experimentId" ]   = db->value( 5 ).toString();
	   run_info[ "runID" ]          = db->value( 6 ).toString();
	   run_info[ "status" ]         = db->value( 7 ).toString();
           run_info[ "dataPath" ]       = db->value( 8 ).toString();   
	   run_info[ "OptimaName" ]     = db->value( 9 ).toString();
	   run_info[ "runStarted" ]     = db->value( 10 ).toString();
	   run_info[ "invID_passed" ]   = db->value( 11 ).toString();

	   run_info[ "correctRadii" ]   = db->value( 13 ).toString();
	   run_info[ "expAborted" ]     = db->value( 14 ).toString();
	   run_info[ "label" ]          = db->value( 15 ).toString();
	   run_info[ "gmpRun" ]         = db->value( 16 ).toString();

	   run_info[ "filename" ]       = db->value( 17 ).toString();
	   run_info[ "aprofileguid" ]   = db->value( 18 ).toString();

	   run_info[ "analysisIDs" ]    = db->value( 19 ).toString();
	   run_info[ "intensityID" ]    = db->value( 20 ).toString();

	   run_info[ "statusID" ]       = db->value( 21 ).toString();
	   	   
	 }
     }

   run_info[ "autoflowID" ]  = QString::number( autoflowID );
   
   qry. clear();
   QString xmlstr( "" );
   US_ProtocolUtil::read_record_auto(  run_info[ "protocolName" ],
				       run_info[ "invID_passed" ].toInt(),
				       &xmlstr, NULL, db );
   QXmlStreamReader xmli( xmlstr );
   US_RunProtocol currProto;
   currProto. fromXml( xmli );

   qDebug() << "Instrument ID from protocol -- " << currProto.rpRotor. instID;
   run_info[ "OptimaID" ] = QString::number( currProto.rpRotor. instID );
   
   return run_info;
}



// get operators from instrumentID
QStringList US_eSignaturesGMP::read_operators( QString optima_id )
{
  QStringList instr_opers;
  
  US_Passwd pw;
  US_DB2* db = new US_DB2( pw.getPasswd() );
  
  if ( db->lastErrno() != US_DB2::OK )
    {
      QMessageBox::warning( this, tr( "LIMS DB Connection Problem" ),
			    tr( "Could not connect to database \n" ) + db->lastError() );

      return instr_opers;
    }

  qDebug() << "OptimaID -- " << optima_id;
  
  //Get operators for this instrID:
  QStringList qry;
  qry << "get_operator_names" << optima_id;
  db->query( qry );

  if ( db->lastErrno() == US_DB2::OK )
    {
      while ( db->next() )
	{
	  int ID    = db->value( 0 ).toString().toInt();
	  //QString GUID  = db->value( 1 ).toString();
	  QString lname = db->value( 2 ).toString();
	  QString fname = db->value( 3 ).toString();
	  
	  instr_opers << QString::number(ID) + ". " + lname + ", " + fname;
	}
    }

  return instr_opers;
}


//read eSign GMP record for assigned oper(s) && rev(s) && status
QMap< QString, QString> US_eSignaturesGMP::read_autoflowGMPReportEsign_record( QString aID)
{
  QMap< QString, QString> eSign_record;
  
  US_Passwd pw;
  US_DB2* db = new US_DB2( pw.getPasswd() );
  
  if ( db->lastErrno() != US_DB2::OK )
    {
      QMessageBox::warning( this, tr( "LIMS DB Connection Problem" ),
			    tr( "Could not connect to database \n" ) + db->lastError() );

      return eSign_record;
    }

  QStringList qry;
  qry << "get_gmp_review_info_by_autoflowID" << aID;
  qDebug() << "read eSing rec, qry -- " << qry;
  
  db->query( qry );

  if ( db->lastErrno() == US_DB2::OK )      // e-Sign record exists
    {
      while ( db->next() )
	{
	  eSign_record[ "ID" ]                   = db->value( 0 ).toString(); 
	  eSign_record[ "autoflowID" ]           = db->value( 1 ).toString();
	  eSign_record[ "autoflowName" ]         = db->value( 2 ).toString();
	  eSign_record[ "operatorListJson" ]     = db->value( 3 ).toString();
	  eSign_record[ "reviewersListJson" ]    = db->value( 4 ).toString();
	  eSign_record[ "eSignStatusJson" ]      = db->value( 5 ).toString();
	  eSign_record[ "eSignStatusAll" ]       = db->value( 6 ).toString();
	  eSign_record[ "createUpdateLogJson" ]  = db->value( 7 ).toString();

	  isEsignRecord = true;
	}
    }
  else
    {
      //No record, so no oper/revs assigned!
      qDebug() << "No e-Sign GMP record exists!!";

      isEsignRecord = false;
      eSign_record. clear();

      // //TEST ---------------------------
      // eSign_record[ "operatorListJson" ]  = QString( tr( "[\"Operator 1\",\"Operator 2\",\"Operator 3\"]" ));
      // eSign_record[ "reviewersListJson" ] = QString( tr( "[\"Reviewer 1\",\"Reviewer 2\",\"Reviewer 3\"]" ));
      // //END TEST ------------------------
    }

  return eSign_record;
}

//Set/Unset  pb_set_operRev -> setEnabled( false );
void US_eSignaturesGMP::setUnsetPb_operRev( void )
{
  QString e_operList = te_opers_to_assign->toPlainText();
  QString e_revList  = te_revs_to_assign->toPlainText();

  if ( e_operList.isEmpty() || e_revList.isEmpty() )
    pb_set_operRev -> setEnabled( false );
  else
    pb_set_operRev -> setEnabled( true );
}

//Add operator to list 
void US_eSignaturesGMP::addOpertoList( void )
{
  QString c_oper = cb_choose_operator->currentText();
  
  //check if selected item already in the list:
  QString e_operList = te_opers_to_assign->toPlainText();
  if ( e_operList. contains( c_oper ) )
    {
      QMessageBox::information( this, tr( "Cannot add Operator" ),
				tr( "<font color='red'><b>ATTENTION:</b> </font> Selected operator: <br><br>"
				    "<font ><b>%1</b><br><br>"
				    "is already in the list of operators.<br>"
				    "Please choose other operator.")
				.arg( c_oper ) );
      return;
    }
  
  te_opers_to_assign->append( c_oper );
  setUnsetPb_operRev();
}

//Remove operator from list 
void US_eSignaturesGMP::removeOperfromList( void )
{
  te_opers_to_assign->setFocus();
  QTextCursor storeCursorPos = te_opers_to_assign->textCursor();
  te_opers_to_assign->moveCursor(QTextCursor::End, QTextCursor::MoveAnchor);
  te_opers_to_assign->moveCursor(QTextCursor::StartOfLine, QTextCursor::MoveAnchor);
  te_opers_to_assign->moveCursor(QTextCursor::End, QTextCursor::KeepAnchor);
  te_opers_to_assign->textCursor().removeSelectedText();
  te_opers_to_assign->textCursor().deletePreviousChar();
  te_opers_to_assign->setTextCursor(storeCursorPos);

  setUnsetPb_operRev();
}

//Add reviewer to list 
void US_eSignaturesGMP::addRevtoList( void )
{
  QString c_rev = cb_choose_rev->currentText();

  //check if selected item already in the list:
  QString e_revList = te_revs_to_assign->toPlainText();
  if ( e_revList. contains( c_rev ) )
    {
      QMessageBox::information( this, tr( "Cannot add Reviewer" ),
				tr( "<font color='red'><b>ATTENTION:</b> </font> Selected reviewer: <br><br>"
				    "<font ><b>%1</b><br><br>"
				    "is already in the list of reviewers.<br>"
				    "Please choose other reviewer.")
				.arg( c_rev ) );
      return;
    }
  
  te_revs_to_assign->append( c_rev );

  setUnsetPb_operRev();
}

//Remove reviewer from list 
void US_eSignaturesGMP::removeRevfromList( void )
{
  te_revs_to_assign->setFocus();
  QTextCursor storeCursorPos = te_revs_to_assign->textCursor();
  te_revs_to_assign->moveCursor(QTextCursor::End, QTextCursor::MoveAnchor);
  te_revs_to_assign->moveCursor(QTextCursor::StartOfLine, QTextCursor::MoveAnchor);
  te_revs_to_assign->moveCursor(QTextCursor::End, QTextCursor::KeepAnchor);
  te_revs_to_assign->textCursor().removeSelectedText();
  te_revs_to_assign->textCursor().deletePreviousChar();
  te_revs_to_assign->setTextCursor(storeCursorPos);

  qDebug() << "Revs to ASSIGN: " << te_revs_to_assign->toPlainText();

  setUnsetPb_operRev();
}

//Assign operators & reviewers for the current GMP run:
void US_eSignaturesGMP::assignOperRevs( void )
{
  /** 
      0. FIRST, check if e-Signature process already BEGAN !!!!         <-- 1st thing to check
        -- if YES, STOP!!!!
	      ** if (isEsignRecord) BUT eSignStatusJson indicates it began
	      ** above && || autoflowStatus indicates 7. e-Signature stage in the signing mode  
	-- if NOT, PROCEED: 
	      ** if (!isEsignRecord)
	      ** if (isEsignRecord) BUT eSignStatusJson HAS NOT began: at least 1 signature put!!!
  ***/		    

  if ( is_eSignProcessBegan() )
    {
      QMessageBox::information( this, tr( "Operator(s), Reviewer(s) CANNOT be Assigned" ),
				tr( "<font color='red'><b>ATTENTION:</b> </font> Operator(s), Reviewer(s)"
				    "cannot be set/changed for the currently uploaded GMP run: <br><br>"
				    "<font ><b>%1</b><br><br>"
				    "E-Signing process has already began.")
				.arg( le_run_name->text() ) );
      return;
    }

  //save existign operators && reviewers:
  QString exsisting_oper_list = te_operator_names->toPlainText();
  QString exsisting_rev_list  = te_reviewer_names->toPlainText();

  //Set new opers && revs in the te areas
  QString oper_list = te_opers_to_assign->toPlainText();
  QString rev_list = te_revs_to_assign->toPlainText();

  //Compose JSON arrays: QString( tr( "[\"Operator 1\",\"Operator 2\",\"Operator 3\"]" ));
                                     
  QString operListJsonArray = "[";
  QString revListJsonArray  = "[";
  QStringList oper_listList = oper_list.split("\n");
  QStringList rev_listList  = rev_list.split("\n");
  QStringList oper_rev_joinedList;

  for (int i=0; i<oper_listList.size(); ++i )
    {
      oper_rev_joinedList << oper_listList[i]; 
      operListJsonArray += "\"" + oper_listList[i] + "\",";
    }
      
  for (int i=0; i<rev_listList.size(); ++i )
    {
      oper_rev_joinedList << rev_listList[i]; 
      revListJsonArray += "\"" + rev_listList[i] + "\",";
    }
  
  operListJsonArray.chop(1);
  revListJsonArray.chop(1);
  operListJsonArray += "]";
  revListJsonArray  += "]";

  qDebug() << "operListJsonArray -- " << operListJsonArray;
  qDebug() << "revListJsonArray -- "  << revListJsonArray;

  //Minimum structure of eSignStatusJson field:
  QString eSignStatusJson = "{\"to_sign\":[";
  for (int i=0; i<oper_rev_joinedList.size(); ++i )
    {
      eSignStatusJson += "\"" + oper_rev_joinedList[i] + "\",";
    }
  eSignStatusJson. chop(1);
  eSignStatusJson += "]}";
  
  qDebug() << "operRevToSignJsonObject -- "  << eSignStatusJson;

  //DB 
  US_Passwd pw;
  US_DB2* db = new US_DB2( pw.getPasswd() );
  
  if ( db->lastErrno() != US_DB2::OK )
    {
      QMessageBox::warning( this, tr( "LIMS DB Connection Problem" ),
			    tr( "Could not connect to database \n" ) + db->lastError() );

      return;
    }
  
  QStringList qry;
  
  
  //Minimum structure of logJson when record created from scratch:
  /** 
      { "Created by": [{ "Person": "12. Savelyev, Alexey", "timeDate": "timestamp", "Comment": "Created frist time" }],
        "Updated by": [{ ... }]  <=== later by admin, e.g. if oper(s), rev(s) are updated
      }
  **/
  QString logJsonFirstTime = "{\"Created by\":[{\"Person\":";

  qry.clear();
  qry <<  QString( "get_user_info" );
  db -> query( qry );
  db -> next();
  int u_ID        = db->value( 0 ).toInt();
  QString u_fname = db->value( 1 ).toString();
  QString u_lname = db->value( 2 ).toString();

  QDateTime date = QDateTime::currentDateTime();
  QString current_date = date.toString("MM-dd-yyyy hh:mm:ss");

  logJsonFirstTime += "\"" + QString::number(u_ID) + ". " + u_lname + ", " + u_fname +  "\",";
  logJsonFirstTime += "\"timeDate\":\"" + current_date +  "\",";
  logJsonFirstTime += "\"Comment\": \"Created first time\"";

  logJsonFirstTime += "}]}";
  qDebug() << "logJsonFirstTimeJsonObject -- "  << logJsonFirstTime;

   
  /**  NEXT, 2 scenarios:
     --- check if eSign record in Db exists;
      1. if YES, update as ADMIN 
        -- update_gmp_review_record_by_admin << p_eSignID       INT,
					     << p_autoflowID    INT,
					     << p_operListJson  TEXT,
                                             << p_revListJson   TEXT,
					     << p_logJson       TEXT
     2. if NOT, create new one: 
        --  new_gmp_review_record  <<	 p_autoflowID   INT,
				   <<	 p_autoflowName TEXT,
				   <<	 p_operListJson TEXT,
				   <<	 p_revListJson  TEXT,
				   <<    p_eSignStatusJson TEXT,
				   <<    p_logJson      TEXT )
			 
     FINALLY. Update autolfow run's gmpReviewID with p_eSignID [if 3. "NO"]
      
   **/

  /***********************************************************************/


  if ( !isEsignRecord ) //No eSignature Record exists, so create new one, with minimal status, createupdatelog
    {
      //Update te fileds
      te_operator_names -> setText( oper_list );
      te_reviewer_names -> setText( rev_list );
      
      int eSignID_returned = 0;
      
      qry. clear();
      qry << "new_gmp_review_record"
      	  << gmp_run_details[ "autoflowID" ]
      	  << gmp_run_details[ "protocolName" ]
      	  << operListJsonArray
      	  << revListJsonArray
      	  << eSignStatusJson       
      	  << logJsonFirstTime;     

      qDebug() << "new_gmp_review_record qry -- " << qry;
      db->statusQuery( qry );
      eSignID_returned = db->lastInsertID();

      if ( eSignID_returned == 0 )
      	{
      	  QMessageBox::warning( this, tr( "New eSign Record Problem" ),
      				tr( "autoflowGMPRecordEsign: There was a problem with creating a new record! \n" ) );
      	  return;
      	}

      //Update primary autolfow record with the new generated eSignID:
      qry. clear();
      qry << "update_autoflow_with_gmpReviewID"
      	  <<  gmp_run_details[ "autoflowID" ]
      	  <<  QString::number( eSignID_returned );

      qDebug() << "update_autoflow_with_gmpReviewID qry -- " << qry;
      db->query( qry );
    }
    
   else //exists eSign record: so update as ADMIN
    {
      qDebug() << "e-Signature record exists: will be updated by ADMIN!!";
      
      qDebug() << "Old Operators -- " << exsisting_oper_list;
      qDebug() << "New Operators -- " << oper_list;
      qDebug() << "Old Reviewers -- " << exsisting_rev_list;
      qDebug() << "New Reviewers -- " << rev_list;
      
      //check if at least one of tehe operator OR reviewer lists has been changed 
      if ( exsisting_oper_list == oper_list && exsisting_rev_list == rev_list )
	{
	  qDebug() << "Operators and Reviewers are the same...";
	  QMessageBox::information( this, tr( "Operator(s), Reviewer(s) NOT CHANGED" ),
				    tr( "Existing and to-be assigned "
					"Operator(s) and Reviewer(s) " 
					"lists are the same.<br><br>"
					"Nothing will be changed..."));
	  
	  return;
	}
      
      QStringList exsisting_oper_listList = exsisting_oper_list.split("\n");
      QStringList exsisting_rev_listList  = exsisting_rev_list .split("\n");

      QMessageBox msg_rev;
      msg_rev.setWindowTitle(tr("Operator(s), Reviewer(s) Change"));
      msg_rev.setText( tr( "<font color='red'><b>ATTENTION:</b> </font><br>"
			   "You are about to change assigned operator(s) and/or reviewer(s): <br><br>"
			   "Old Operators: <font ><b>%1</b><br>"
			   "New Operators: <font ><b>%2</b><br><br>"
			   "Old Reviewers: <font ><b>%3</b><br>"
			   "New Reviewers: <font ><b>%4</b><br><br>")
		       .arg( exsisting_oper_listList.join(",") )
		       .arg( oper_listList.join(",") )
		       .arg( exsisting_rev_listList.join(",") )
		       .arg( rev_listList.join(",") )
		       );
      
      msg_rev.setInformativeText( QString( tr( "Do you want to Proceed?" )));
      
      QPushButton *Accept    = msg_rev.addButton(tr("Proceed"), QMessageBox::YesRole);
      QPushButton *Cancel    = msg_rev.addButton(tr("Cancel"), QMessageBox::RejectRole);
      
      msg_rev.setIcon(QMessageBox::Question);
      msg_rev.exec();
      
      if (msg_rev.clickedButton() == Accept)
	{
	  //Update te fileds
	  te_operator_names -> setText( oper_list );
	  te_reviewer_names -> setText( rev_list );
	  
	  //Minimum structure JSON for logJsonUpdateTime:
	  QString logJsonUpdateTime = compose_updated_admin_logJson( u_ID, u_fname, u_lname );
	  qDebug() << "logJsonUpdateTimeJsonObject -- "  << logJsonUpdateTime;
	  
	  qry. clear();
	  qry << "update_gmp_review_record_by_admin"
	      << eSign_details[ "ID" ]
	      << gmp_run_details[ "autoflowID" ]
	      << operListJsonArray
	      << revListJsonArray
	      << logJsonUpdateTime;

	  qDebug() << "update_gmp_review_record_by_admin, qry -- " << qry;
	  db->query( qry );
	}
      else
	{
	  qDebug() << "Canceling oper(s)/rev(s) change...";
	  return;
	}
    }

  /**************************************************/

}

//Check if e-Signing process for the GMP run started
bool US_eSignaturesGMP::is_eSignProcessBegan( void )
{
  bool isBegan = false;
  eSign_details .clear();
  eSign_details = read_autoflowGMPReportEsign_record( gmp_run_details[ "autoflowID" ] );

  if ( !isEsignRecord )
    return isBegan;

  QString operatorListJson  = eSign_details[ "operatorListJson" ];
  QString reviewersListJson = eSign_details[ "reviewersListJson" ];
  QString eSignStatusJson   = eSign_details[ "eSignStatusJson" ];

  /****
     Proposed JSON Struncture of eSignStatusJson:
     { 
        "to_sign": ["Rev1","Rev2"],       <===== ORIGINALLY, full combined list of Oper(s) && Rev(s)
	                                         i.e. ["Oper1","Oper2",..., "Rev1","Rev2",..]
	"signed" : [
	              { "Oper1": 
	                      { 
			        "Comment"  : "Explanation",
			        "timeData" : "timestamp"
			      }},
		      { "Oper2" :
		              {
			        Same ...
			      }}
		    ]
     }

     So, to understand if e-Signing started, investigate eSignStatusJson[ "signed" ] JSON...
   ****/
  QJsonDocument jsonDoc = QJsonDocument::fromJson( eSignStatusJson.toUtf8() );
  if (!jsonDoc.isObject())
    {
      qDebug() << "is_eSignProcessBegan(): eSignStatusJson: NOT a JSON Doc !!";
      return isBegan;
    }
  
  const QJsonValue &to_esign = jsonDoc.object().value("to_sign");
  const QJsonValue &esigned  = jsonDoc.object().value("signed");

  QJsonArray to_esign_array  = to_esign .toArray();
  QJsonArray esigned_array   = esigned  .toArray();

  //to_sign:
  if ( to_esign.isUndefined() )
    qDebug() << "All signatures have been collected; noone left to e-sign !!";

  //signed
  if ( esigned.isUndefined() || esigned_array.size() == 0 || !esigned_array.size() )
    {
      qDebug() << "Nothing has been e-Signed yet !!! Oper(s), Rev(s) CAN BE changed/assigned!!!";
      return false;
    }
  else
    {
      //DEBUG
      for (int i=0; i < esigned_array.size(); ++i )
	{
	  foreach(const QString& key, esigned_array[i].toObject().keys())
	    {
	      QJsonObject newObj = esigned_array[i].toObject().value(key).toObject();
	      
	      qDebug() << "E-Signed - " << key << ": Comment, timeData -- "
		       << newObj["Comment"]   .toString()
		       << newObj["timeData"]  .toString();
	    }
	}
      // END DEBUG: There is/are e-Signee already; 
      isBegan = true;
    }
  return isBegan;
}


//append eSign log Json by admin upon opers/rev Update:
QString US_eSignaturesGMP::compose_updated_admin_logJson( int u_ID, QString u_fname, QString u_lname )
{
  QString e_logJson   = eSign_details[ "createUpdateLogJson" ];

  QJsonDocument jsonDoc = QJsonDocument::fromJson( e_logJson.toUtf8() );
  if (!jsonDoc.isObject())
    {
      qDebug() << "compose_updated_admin_logJson(): createUpdateLogJson: NOT a JSON Doc !!";
      return e_logJson;
    }

  //Appended portion
  QString logJsonUpdateTime = ",\"Updated by\":[{\"Person\":";

  QDateTime date = QDateTime::currentDateTime();
  QString current_date = date.toString("MM-dd-yyyy hh:mm:ss");

  logJsonUpdateTime += "\"" + QString::number(u_ID) + ". " + u_lname + ", " + u_fname +  "\",";
  logJsonUpdateTime += "\"timeDate\":\"" + current_date +  "\",";
  logJsonUpdateTime += "\"Comment\": \"Updated Operator, Reviewer lists\"";

  logJsonUpdateTime += "}]}";

  //Combined JSON
  e_logJson.chop(1); //remove trailing '}'
  QString composedJson = e_logJson + logJsonUpdateTime;

  return composedJson;
}
