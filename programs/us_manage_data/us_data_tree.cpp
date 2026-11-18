//! \file us_data_tree.cpp

#include "us_data_tree.h"
#include "us_settings.h"
#include "us_gui_settings.h"
#include "us_constants.h"
#include "us_editor.h"
#include "us_noise.h"
#include "us_util.h"

#include <QScopedPointer>

const QColor colorRed(   240,   0,   0 );
const QColor colorBlue(    0,   0, 255 );
const QColor colorBrown( 120,  60,   0 );
const QColor colorGreen(   0, 150,   0 );
const QColor colorGray(  110, 110, 110 );
const QColor colorTeal(    0, 120, 140 );
const QColor colorWhite( 255, 255, 240 );

// class to manage a tree widget, underlying data, and processing
US_DataTree::US_DataTree( US_DataModel* dmodel, QTreeWidget* treewidg,
      QWidget* parent /*=0*/ )
{
   da_model   = dmodel;
   tw_recs    = treewidg;
   parentw    = (QWidget*)parent;
   da_process = NULL;
   tw_item    = NULL;
   dbg_level  = US_Settings::us_debug();
   gmp_models_read = false;

   QStringList theads;

   theads << "Type" << "Label" << "SubType" << "Source"
          << "Children\nDB, Local" << "Descendants\nDB, Local";
   ntcols = theads.size();
   tw_recs->setHeaderLabels( theads );
   tw_recs->setFont(  QFont( US_Widgets::fixedFont().family(),
                      US_GuiSettings::fontSize() - 1 ) );
   tw_recs->setObjectName( QString( "tree-widget" ) );
   tw_recs->setAutoFillBackground( true );
}

// show or hide (expand/collapse) all rows of specified type
void US_DataTree::toggle_expand( QString c1str, bool show )
{
   QList< QTreeWidgetItem* > listi = tw_recs->findItems(
      c1str, Qt::MatchExactly | Qt::MatchWrap | Qt::MatchRecursive, 0 );

DbgLv(1) << "DT: togl_exp str show" << c1str << show;
   for ( int ii = 0; ii < listi.size(); ii++ )
      listi.at( ii )->setExpanded( show );
}

// The color, type name and source text of one record
void US_DataTree::row_look( const US_DataModel::DataDesc& desc,
                            QBrush& fbru, QString& rtyp, QString& rsrc )
{
   static const char* names[] =
      { "Experiment", "Raw", "Edited", "Model", "Noise" };

   int  ityp  = desc.recType;
   rtyp       = ( ityp >= 0  &&  ityp <= 4 ) ? names[ ityp ] : "none";

   switch ( US_DataModel::sync_state( desc ) )
   {
      case US_DataModel::SYNC_LOCAL:
         fbru    = QBrush( colorBrown );
         rsrc    = "Local";
         break;

      case US_DataModel::SYNC_DB:
         fbru    = QBrush( colorBlue );
         rsrc    = "DB";
         break;

      case US_DataModel::SYNC_UNVERIFIED:
         fbru    = QBrush( colorTeal );
         rsrc    = "Unverified";
         break;

      case US_DataModel::SYNC_INSYNC:
         fbru    = QBrush( colorGreen );
         rsrc    = "In Sync";
         break;

      case US_DataModel::SYNC_CONFLICT:
         fbru    = QBrush( colorRed );
         rsrc    = "Conflict";

         // A model or noise whose bytes differ may differ only in how its
         // values were written out
         if ( ( ityp == US_DataModel::MODEL  ||  ityp == US_DataModel::NOISE )
              &&  da_model->dbase() != NULL
              &&  ! significant_diffs( desc, da_model->dbase() ) )
         {
            fbru = QBrush( colorGreen );
            rsrc = "In Sync";
         }
         break;

      case US_DataModel::SYNC_DUMMY:
      default:
         fbru    = QBrush( colorGray );
         rsrc    = "dummy";
         break;
   }
}

// build the first layer of the data tree:  one row per experiment
//
// The rows below an experiment are added by fill_run(), when the scan
// reaches that experiment or when the user opens it.  A store of
// multi-wavelength runs holds hundreds of triples per experiment, so
// building all of that before showing anything is what made this window
// feel slow.
void US_DataTree::build_dtree( )
{
   da_process = (US_DataProcess*)da_model->procobj();

   tw_recs->clear();
   runitems.clear();

   int nruns  = da_model->runCount();

   for ( int ii = 0; ii < nruns; ii++ )
      add_run( ii );

   for ( int jj = 0; jj < ntcols; jj++ )
      tw_recs->resizeColumnToContents( jj );
}

// add the row of one experiment, and its records when they are read
void US_DataTree::add_run( int runIndex )
{
   if ( runIndex != runitems.size() )   return;   // in order, once each

   US_DataModel::RunEntry entry = da_model->run_entry( runIndex );
   US_DataModel::DataDesc desc  = da_model->row_datadesc( entry.row );

   QBrush  fbru( colorBrown );
   QString rtyp;
   QString rsrc;
   row_look( desc, fbru, rtyp, rsrc );

   // A source that could not count the chain yet says so with a question
   // mark rather than a zero that is not true
   QString counts = ( entry.dbCount < 0 ? QString( "?" )
                                        : QString::number( entry.dbCount ) )
                    + ", "
                    + ( entry.loCount < 0 ? QString( "?" )
                                          : QString::number( entry.loCount ) );

   QStringList cvals;
   cvals << rtyp << desc.label << desc.subType << rsrc << "" << counts;

   QTreeWidgetItem* item = new QTreeWidgetItem(
      tw_recs, cvals, (int)QTreeWidgetItem::UserType + entry.row );

   for ( int jj = 0; jj < ntcols; jj++ )
   {
      item->setForeground( jj, fbru );
      item->setBackground( jj, QBrush( colorWhite ) );
   }

   if ( ! entry.loaded )
      item->setChildIndicatorPolicy( QTreeWidgetItem::ShowIndicator );

   // A run that should not be acted on as it is says why
   if ( ! entry.problem.isEmpty() )
   {
      QString tip = tr( "This run has a problem:\n%1\n\n"
                        "Run IDs with dots in them are from older versions"
                        " of UltraScan; new runs may use only letters,"
                        " digits, '_' and '-'." ).arg( entry.problem );

      for ( int jj = 0; jj < ntcols; jj++ )
         item->setToolTip( jj, tip );

      item->setText( 1, desc.label + " (!)" );
   }

   else if ( entry.orphans )
   {
      for ( int jj = 0; jj < ntcols; jj++ )
         item->setToolTip( jj, tr( "Records whose experiment or parent"
                                   " record is missing; the gray rows stand"
                                   " in for the missing parents." ) );
   }

   runitems << item;

   if ( entry.loaded )
      fill_run( runIndex );
}

// add the records of one experiment under its row
void US_DataTree::fill_run( int runIndex )
{
   if ( runIndex < 0  ||  runIndex >= runitems.size() )   return;

   US_DataModel::RunEntry entry = da_model->run_entry( runIndex );

   if ( ! entry.loaded )                                  return;

   QTreeWidgetItem* runitem = runitems[ runIndex ];

   if ( runitem->childCount() > 0 )                       return;

   runitem->setChildIndicatorPolicy(
            QTreeWidgetItem::DontShowIndicatorWhenChildless );

   QTreeWidgetItem* pitems[ 5 ];
   pitems[ 0 ] = runitem;
   pitems[ 1 ] = runitem;
   pitems[ 2 ] = runitem;
   pitems[ 3 ] = runitem;
   pitems[ 4 ] = runitem;

   int nchdb  = 0;
   int nchlo  = 0;

   for ( int row = entry.firstRow; row >= 0  &&  row <= entry.lastRow; row++ )
   {
      US_DataModel::DataDesc desc = da_model->row_datadesc( row );
      int     ityp = desc.recType;

      if ( ityp < 1  ||  ityp > 4 )   continue;

      QBrush  fbru( colorBrown );
      QString rtyp;
      QString rsrc;
      row_look( desc, fbru, rtyp, rsrc );

      // count the children and the descendants of this record
      int kchdb = 0;
      int kchlo = 0;
      int kdedb = 0;
      int kdelo = 0;

      for ( int jj = row + 1; jj <= entry.lastRow; jj++ )
      {
         US_DataModel::DataDesc ddesc = da_model->row_datadesc( jj );

         if ( ddesc.recType <= ityp )   break;

         bool isDba = ( ( ddesc.recState & US_DataModel::REC_DB ) != 0 );
         bool isLoc = ( ( ddesc.recState & US_DataModel::REC_LO ) != 0 );
         bool child = ( ( ddesc.recType - ityp ) == 1 );

         kdedb   += ( isDba          ) ? 1 : 0;
         kchdb   += ( isDba && child ) ? 1 : 0;
         kdelo   += ( isLoc          ) ? 1 : 0;
         kchlo   += ( isLoc && child ) ? 1 : 0;
      }

      if ( ityp == US_DataModel::RAW )
      {  // the raws are the children of the experiment
         nchdb  += ( ( desc.recState & US_DataModel::REC_DB ) != 0 ) ? 1 : 0;
         nchlo  += ( ( desc.recState & US_DataModel::REC_LO ) != 0 ) ? 1 : 0;
      }

      QString anch = ( ityp < 4 ) ?
         QString( "%1, %2" ).arg( kchdb ).arg( kchlo ) : "";
      QString ande = ( ityp < 3 ) ?
         QString( "%1, %2" ).arg( kdedb ).arg( kdelo ) : "";

      QStringList cvals;
      cvals << rtyp << desc.label << desc.subType << rsrc << anch << ande;

      QTreeWidgetItem* item = new QTreeWidgetItem(
         pitems[ ityp - 1 ], cvals, (int)QTreeWidgetItem::UserType + row );

      pitems[ ityp ] = item;

      for ( int jj = 0; jj < ntcols; jj++ )
      {
         item->setForeground( jj, fbru );
         item->setBackground( jj, QBrush( colorWhite ) );
      }
   }

   // the counts on the experiment row are now what is really under it --
   // the model counted them the same way when it merged the experiment
   runitem->setText( 4, QString( "%1, %2" ).arg( nchdb ).arg( nchlo ) );
   runitem->setText( 5, QString( "%1, %2" ).arg( entry.dbCount )
                                           .arg( entry.loCount ) );
}

/* Colour one experiment's rows again.

   A scan reads the checksums after the tree is built, so a row that read
   as unverified becomes in sync or in conflict once its records have
   actually been compared -- and an action changes where a record is.
*/
void US_DataTree::refresh_run( int runIndex )
{
   if ( runIndex < 0  ||  runIndex >= runitems.size() )   return;

   US_DataModel::RunEntry entry = da_model->run_entry( runIndex );

   if ( ! entry.loaded )                                  return;

   QList< QTreeWidgetItem* > stack;
   stack << runitems[ runIndex ];

   while ( ! stack.isEmpty() )
   {
      QTreeWidgetItem* item = stack.takeFirst();

      for ( int ii = 0; ii < item->childCount(); ii++ )
         stack << item->child( ii );

      int row = item->type() - (int)QTreeWidgetItem::UserType;

      if ( row < 0  ||  row >= da_model->recCount() )   continue;

      US_DataModel::DataDesc desc = da_model->row_datadesc( row );

      QBrush  fbru( colorBrown );
      QString rtyp;
      QString rsrc;

      row_look( desc, fbru, rtyp, rsrc );

      item->setText( 3, rsrc );

      for ( int jj = 0; jj < ntcols; jj++ )
         item->setForeground( jj, fbru );
   }
}

// make sure an experiment's records are read, outside a scan
bool US_DataTree::ensure_loaded( int runIndex )
{
   if ( da_model->run_entry( runIndex ).loaded )   return true;

   if ( ! da_model->scan_run( runIndex ) )        return false;

   fill_run( runIndex );
   return true;
}

// read an experiment the user opened, if the scan has not reached it yet
void US_DataTree::item_opened( QTreeWidgetItem* item )
{
   if ( item == NULL )                            return;
   if ( item->parent() != NULL )                  return;  // not an experiment

   int runIndex = runitems.indexOf( item );

   if ( runIndex < 0 )                            return;

   if ( da_model->scan_active()  ||  da_model->is_busy() )
   {  // A scan is running: what the user opened goes to the front of its
      // queues, and the scan loop gets to it next.  Reading it here would
      // run one read inside another.
      da_model->request_run   ( runIndex );
      da_model->request_verify( runIndex );
      return;
   }

   ensure_loaded( runIndex );

   // Comparing is the slow part; it is only queued here, and whoever runs
   // the queue does it without holding up the expansion
   da_model->request_verify( runIndex );
}

// set up and display a row context menu
void US_DataTree::row_context_menu( QTreeWidgetItem* item )
{
   if ( item == NULL )                                     return;
   if ( da_model->scan_active()  ||  da_model->is_busy() ) return;

   selitems = tw_recs->selectedItems();

   if ( ! selitems.contains( item ) )
      selitems << item;

   int nsel = selitems.size();
DbgLv(1) << "    context_menu nbr sel rows" << selitems.size();
   tw_item  = item;
   int irow = item->type() - (int)QTreeWidgetItem::UserType;

   // An experiment acts on its records, so they have to be read
   for ( int ii = 0; ii < nsel; ii++ )
   {
      if ( selitems[ ii ]->parent() == NULL )
         ensure_loaded( runitems.indexOf( selitems[ ii ] ) );
   }

   if ( nsel > 1 )
   {  // If multiple selections, set first row
      for ( int ii = 0; ii < nsel; ii++ )
      {
         QTreeWidgetItem* jitem = selitems[ ii ];
         int jrow = jitem->type() - (int)QTreeWidgetItem::UserType;
         irow     = qMin( irow, jrow );
      }
   }

DbgLv(2) << "    context_menu row" << irow+1;
   da_model->setCurrent( irow );
   cdesc    = da_model->current_datadesc();

   QString tupload = tr( " upload record to DB" );
   QString tdnload = tr( " download record to local" );
   QString tdbasrm = tr( " remove branch from DB" );
   QString tloclrm = tr( " remove branch from local" );
   QString tbothrm = tr( " remove branch from both" );
   QString tshdeta = tr( " show record details" );

   if ( nsel > 1 )
   {  // Multiple selections:  "record/branch" to "branches"
      tupload.replace( tr( "record" ), tr( "branches" ) );
      tdnload.replace( tr( "record" ), tr( "branches" ) );
      tdbasrm.replace( tr( "branch" ), tr( "branches" ) );
      tloclrm.replace( tr( "branch" ), tr( "branches" ) );
      tbothrm.replace( tr( "branch" ), tr( "branches" ) );
      tshdeta.replace( tr( "record" ), tr( "records"  ) );
   }

   if ( cdesc.recType <= US_DataModel::RAW )
   {  // Experiment or Raw:  "record/branches" to "descendants"
      tupload.replace( tr( "record"   ), tr( "descendants" ) );
      tupload.replace( tr( "branches" ), tr( "descendants" ) );
      tdnload.replace( tr( "record"   ), tr( "descendants" ) );
      tdnload.replace( tr( "branches" ), tr( "descendants" ) );
      tdbasrm.replace( tr( "branches" ), tr( "descendants" ) );
      tdbasrm.replace( tr( "branch"   ), tr( "descendants" ) );
      tloclrm.replace( tr( "branches" ), tr( "descendants" ) );
      tloclrm.replace( tr( "branch"   ), tr( "descendants" ) );
      tbothrm.replace( tr( "branches" ), tr( "descendants" ) );
      tbothrm.replace( tr( "branch"   ), tr( "descendants" ) );
   }

   // The menu and its actions live only as long as this call
   QMenu    cmenu;
   QAction* aupload = cmenu.addAction( tupload );
   QAction* adnload = cmenu.addAction( tdnload );
   QAction* adbasrm = cmenu.addAction( tdbasrm );
   QAction* aloclrm = cmenu.addAction( tloclrm );
   QAction* abothrm = cmenu.addAction( tbothrm );
   QAction* ashdeta = cmenu.addAction( tshdeta );

   // disable menu items that are not appropriate to the record

   if ( ( cdesc.recState & US_DataModel::REC_LO ) == 0 )
   {  // if record not local, no upload and no remove from local or both
      aupload->setEnabled( false );
      aloclrm->setEnabled( false );
      abothrm->setEnabled( false );
   }

   if ( ( cdesc.recState & US_DataModel::REC_DB ) == 0 )
   {  // if record not db, no download and no remove from db or both
      adnload->setEnabled( false );
      adbasrm->setEnabled( false );
      abothrm->setEnabled( false );
   }

   // Upload needs the parent in the DB.  An experiment or a raw record is
   // not uploaded itself -- its descendants are -- so for those it is the
   // record itself that has to be in the DB for them to have a parent there.
   int prow = ( cdesc.recType <= US_DataModel::RAW )
            ? irow : da_model->parent_row( irow );

   if ( prow < 0  ||
        ( da_model->row_datadesc( prow ).recState
          & US_DataModel::REC_DB ) == 0 )
      aupload->setEnabled( false );

   // display the context menu and act on selection
   QAction* chosen = cmenu.exec( QCursor::pos() );

   if      ( chosen == NULL )      return;
   else if ( chosen == aupload )   item_upload();
   else if ( chosen == adnload )   item_download();
   else if ( chosen == adbasrm )   item_remove_db();
   else if ( chosen == aloclrm )   item_remove_loc();
   else if ( chosen == abothrm )   item_remove_all();
   else if ( chosen == ashdeta )   item_details();
}

// open a dialog and display data tree help
void US_DataTree::dtree_help()
{
   QString fileexts = tr( "Text,Log files (*.txt *.log);;" )
      + tr( "All files (*)" );
   QString mtext =
      tr( "Data Tree Columns --\n\n" ) +
      tr( " \"Type\"       :  Type of data set record -\n" ) +
      tr( "                  \"Experiment\", \"Raw\", \"Edited\",\n" ) +
      tr( "                  \"Model\" or \"Noise\".  The experiments are\n" ) +
      tr( "                  the top level; everything else hangs off one.\n" ) +
      tr( " \"Label\"      :  Short description of specific record.\n" ) +
      tr( " \"SubType\"    :  Record-specific type (e.g. \"2DSA\", \"TI\").\n")+
      tr( " \"Source\"     :  Location/state (see color legend below)-\n" ) +
      tr( "                  \"DB\"         (Blue,  database); \n" ) +
      tr( "                  \"Local\"      (Brown, local disk); \n" ) +
      tr( "                  \"Unverified\" (Teal,  both, not compared yet);\n" ) +
      tr( "                  \"In Sync\"    (Green, both, consistent); \n" ) +
      tr( "                  \"Conflict\"   (Red,   both, inconsistent); \n" ) +
      tr( "                  \"dummy\"      (Gray,  missing parent filler).\n") +
      tr( " \"Children    :  Number of Children of this row\n" ) +
      tr( "  DB, Local\"       from Database, Local-disk.\n" ) +
      tr( " \"Descendants :  Number of Descendants of this row\n" ) +
      tr( "  DB, Local\"       from Database, Local-disk.  On an experiment\n" ) +
      tr( "                    not read yet this is what the listing says\n" ) +
      tr( "                    is under it (\"?\" when it cannot say).\n\n" ) +
      tr( "Tree Row Color Legend --\n\n" ) +
      tr( "  Blue   :  This record exists for database only.\n" ) +
      tr( "  Brown  :  This record exists for local disk only.\n" ) +
      tr( "  Teal   :  This record exists for both, and its two copies\n" ) +
      tr( "              have not been compared yet.  A scan compares\n" ) +
      tr( "              them after the tree is complete.\n" ) +
      tr( "  Green  :  This record exists and is consistent\n" ) +
      tr( "              for both database and local.\n" ) +
      tr( "  Red    :  This record exists for both, but is inconsistent,\n")+
      tr( "              or one copy could not be read to compare it.\n")+
      tr( "  Gray   :  This record is a filler for a missing parent.\n" ) +
      tr( "              Records whose parent is missing are shown under\n" ) +
      tr( "              such fillers, under a last experiment row,\n" ) +
      tr( "              \"(records of no experiment)\", so they can still\n" ) +
      tr( "              be removed.  A run marked \"(!)\" has a problem;\n" ) +
      tr( "              hold the mouse over it to see which.  Run IDs\n" ) +
      tr( "              with dots are legacy: still shown, no longer\n" ) +
      tr( "              accepted for new runs.\n\n" ) +
      tr( "Scan Progress --\n\n" ) +
      tr( "  The upper bar counts the steps of the phase that is running\n" ) +
      tr( "  (listing, reading, comparing); the lower bar the work inside\n" ) +
      tr( "  the experiment being read.  Opening an experiment while a\n" ) +
      tr( "  scan runs moves it to the front of the queue.\n\n" ) +
      tr( "Tree Entry Processes --\n\n" ) +
      tr( "  A right-mouse-button click on any cell of a row pops up\n" ) +
      tr( "  a context menu with actions to take for the record.\n" ) +
      tr( "  Menu items will only be enabled if the action is\n" ) +
      tr( "  appropriate for the particular record shown in the row.\n" ) +
      tr( "  On an experiment or a raw record, an action applies to\n" ) +
      tr( "  everything under it.  No action is offered while a scan runs.\n" ) +
      tr( "  Possible actions are:\n" ) +
      tr( "    \"upload to DB\"\n" ) +
      tr( "       -> Upload this local record to the database;\n" ) +
      tr( "    \"download to local\"\n" ) +
      tr( "       -> Download this DB record to local disk.\n" ) +
      tr( "    \"remove from DB\"\n" ) +
      tr( "       -> Remove this record from the database.\n" ) +
      tr( "    \"remove from local\"\n" ) +
      tr( "       -> Remove this record from local disk.\n" ) +
      tr( "    \"remove both\"\n" ) +
      tr( "       -> Remove this record from both DB and local.\n" ) +
      tr( "    \"show details\"\n" ) +
      tr( "       -> Pop up a text dialog with details about the record.\n\n" ) +
      tr( "Detailed Help --\n\n" ) +
      tr( "  Click on the \"Help\" button for standard documentation,\n" ) +
      tr( "  including sample images and details on all GUI elements.\n" );

   US_Editor* editd = new US_Editor( US_Editor::LOAD, true, fileexts );
   editd->setWindowTitle( tr( "Data Sets Tree Help and Legend" ) );
   editd->move( QCursor::pos() + QPoint( 200, 200 ) );
   editd->resize( 600, 500 );
   editd->e->setFont( QFont( US_Widgets::fixedFont().family(),
                      US_GuiSettings::fontSize() ) );
   editd->e->setText( mtext );
   editd->show();
}

// Ask whether to go ahead with an action, then do it and report
void US_DataTree::confirm_and_act( QString item_exs, QString item_act )
{
   QMessageBox msgBox( parentw );
   int irow = tw_item->type() - (int)QTreeWidgetItem::UserType;
   da_model->setCurrent( irow );
   cdesc    = da_model->current_datadesc();

   record_type( cdesc.recType, item_act );

   msgBox.setWindowTitle( item_act );
   msgBox.setTextFormat(  Qt::RichText );
   msgBox.setText( action_text( item_exs, item_act ) );
   msgBox.addButton( QMessageBox::No );
   msgBox.addButton( QMessageBox::Yes );
   msgBox.setDefaultButton( QMessageBox::No );

   if ( msgBox.exec() == QMessageBox::Yes )
      action_result( do_actions( item_exs, item_act ), item_act );

   else
      action_result( ACT_CANCELLED, item_act );
}

// perform item upload action
void US_DataTree::item_upload()
{
   int irow = tw_item->type() - (int)QTreeWidgetItem::UserType;
   bool both = ( ( da_model->row_datadesc( irow ).recState
                   & US_DataModel::REC_DB ) != 0 );

   confirm_and_act( both ? tr( "both DB and Local" ) : tr( "Local disk only" ),
                    both ? tr( "DB replace" )        : tr( "DB create" ) );
}

// perform item download action
void US_DataTree::item_download()
{
   int irow = tw_item->type() - (int)QTreeWidgetItem::UserType;
   bool both = ( ( da_model->row_datadesc( irow ).recState
                   & US_DataModel::REC_LO ) != 0 );

   confirm_and_act( both ? tr( "both DB and Local" ) : tr( "Database only" ),
                    both ? tr( "Local file replace" )
                         : tr( "Local file create" ) );
}

// perform item remove-from-db action
void US_DataTree::item_remove_db()
{
   confirm_and_act( tr( "Database only" ), tr( "DB-only remove" ) );
}

// perform item remove-from-local action
void US_DataTree::item_remove_loc()
{
   confirm_and_act( tr( "Local disk only" ), tr( "Local-only remove" ) );
}

// perform item remove-from-all action
void US_DataTree::item_remove_all()
{
   confirm_and_act( tr( "both DB and Local" ), tr( "DB+Local remove" ) );
}

// Perform item details action
void US_DataTree::item_details(  )
{
   if ( selitems.size() > 1 )
   {
      items_details();
      return;
   }

   const char* rtyps[]  = { "Experiment", "RawData", "EditedData",
                            "Model", "Noise" };
   QString     fileexts = tr( "Text,Log files (*.txt *.log)"
                              ";;All files (*)" );
   int         irow     = tw_item->type() - (int)QTreeWidgetItem::UserType;
DbgLv(2) << "DT: i_details row" << irow;

   da_model->setCurrent( irow++ );   // get description record, index as 1...
   cdesc    = da_model->current_datadesc();

   QString mtext =
      tr( "Data Tree Item at Row %1 -- \n\n" ).arg( irow ) +
      tr( "  Type           : %1 (%2)\n" )
         .arg( cdesc.recType ).arg( rtyps[ cdesc.recType ] ) +
      tr( "  SubType        : " ) + cdesc.subType + "\n" +
      tr( "  Label          : " ) + cdesc.label + "\n" +
      tr( "  Description    : " ) + cdesc.description + "\n" +
      tr( "  DB record ID   : %1\n" ).arg( cdesc.recordID ) +
      tr( "  Global ID      : " ) + cdesc.dataGUID + "\n" +
      tr( "  Parent GUID    : " ) + cdesc.parentGUID + "\n" +
      tr( "  DB parent ID   : %1\n" ).arg( cdesc.parentID ) +
      tr( "  File Directory : " )
         + cdesc.filename.section( "/",  0, -2 ) + "\n" +
      tr( "  File Name      : " )
         + cdesc.filename.section( "/", -1, -1 ) + "\n" +
      tr( "  File Last Mod  : " ) + cdesc.filemodDate + "\n" +
      tr( "  DB   Last Mod  : " ) + cdesc.lastmodDate + "\n" +
      tr( "  Record State   : " ) + record_state( cdesc.recState ) + "\n";

   if ( cdesc.contents.length() < 60 )
   {
      mtext = mtext + 
         tr( "  Content Checks : " ) + cdesc.contents;
   }
   else
   {
      QString cont1 = cdesc.contents.section( " ", 0, 1 ).simplified();
      QString cont2 = cdesc.contents.section( " ", 2, 4 ).simplified();
      mtext = mtext + 
         tr( "  Content Checks : " ) + cont1 + "\n" +
             "                   "   + cont2 + "\n";
   }

   // display the text dialog
   US_Editor* editd = new US_Editor( US_Editor::LOAD, true, fileexts );
   editd->setWindowTitle( tr( "Data Tree Entry Details" ) );
   editd->move( QCursor::pos() + QPoint( 100, 100 ) );
   editd->resize( 720, 360 );
   editd->e->setFont( QFont( US_Widgets::fixedFont().family(),
                      US_GuiSettings::fontSize() ) );
   editd->e->setText( mtext );
   editd->show();
}

// Perform multiple-items details action
void US_DataTree::items_details(  )
{
   const char* rtyps[]  = { "Experiment", "RawData", "EditedData",
                            "Model", "Noise" };
   QString     fileexts = tr( "Text,Log files (*.txt *.log)"
                              ";;All files (*)" );

   int narows = action_rows();

   int nsrows = selrows.size();
   int nrrows = rawrows.size();
   int irow   = selrows[ 0 ];
   int krow   = selrows[ nsrows - 1 ];
   krow       = ( narows > 0 ) ? actrows[ narows - 1 ] : krow;
DbgLv(2) << "DT: i_details row" << irow;
   cdesc    = da_model->row_datadesc( irow );
   US_DataModel::DataDesc kdesc    = da_model->row_datadesc( krow );

   QString mtext =
      tr( "Data Tree Items from Rows %1 to %2 -- \n\n" )
         .arg( irow ).arg( krow ) +
      tr( "  Type   (first) : %1\n" ).arg( rtyps[ cdesc.recType ] ) +
      tr( "    Description    : " ) + cdesc.description + "\n" +
      tr( "    DB record ID   : %1\n" ).arg( cdesc.recordID ) +
      tr( "    File Directory : " )
         + cdesc.filename.section( "/",  0, -2 ) + "\n" +
      tr( "    File Name      : " )
         + cdesc.filename.section( "/", -1, -1 ) + "\n" +
      tr( "  Type    (last) : %1\n" ).arg( rtyps[ kdesc.recType ] ) +
      tr( "    Description    : " ) + kdesc.description + "\n" +
      tr( "    DB record ID   : %1\n" ).arg( kdesc.recordID ) +
      tr( "    File Directory : " )
         + kdesc.filename.section( "/",  0, -2 ) + "\n" +
      tr( "    File Name      : " )
         + kdesc.filename.section( "/", -1, -1 ) + "\n" +
      tr( "  Total Selected Records : " ) + QString::number( nsrows ) + "\n" +
      tr( "  Total Action Records   : " ) + QString::number( narows ) + "\n" +
      tr( "  Total Raw Selections   : " ) + QString::number( nrrows ) + "\n";

   // display the text dialog
   US_Editor* editd = new US_Editor( US_Editor::LOAD, true, fileexts );
   editd->setWindowTitle( tr( "Data Tree Entries Details" ) );
   editd->move( QCursor::pos() + QPoint( 100, 100 ) );
   editd->resize( 760, 320 );
   editd->e->setFont( QFont( US_Widgets::fixedFont().family(),
                      US_GuiSettings::fontSize() ) );
   editd->e->setText( mtext );
   editd->show();
}

// Prepend a record type string to an item action
void US_DataTree::record_type( int recType, QString& item_act )
{
   const char* rtyps[]  = { "Experiment", "RawData", "EditedData",
                            "Model", "Noise" };
   int sizert = sizeof( rtyps ) / sizeof( rtyps[ 0 ] );

   if ( recType >= 0  &&  recType < sizert )
   {
      item_act = QString( rtyps[ recType ] ) + " " + item_act;
   }
}

// Format an item action text for a message box
QString US_DataTree::action_text( QString exstext, QString acttext )
{
   QString lines = ( selitems.size() == 1 )
                 ?  tr( "This item exists on %1.<br>"
                        "Are you sure you want to proceed with a %2?<ul>" )
                    .arg( exstext ).arg( acttext )
                 :  tr( "These items exists on %1.<br>"
                        "Are you sure you want to proceed with %2s?<ul>" )
                    .arg( exstext ).arg( acttext );
   return lines +
           tr( "<li><b>No </b> to cancel the action;</li>"
               "<li><b>Yes</b> to proceed with the action.</li></ul>" );
}

// report the result of an item action
void US_DataTree::action_result( int stat, QString item_act )
{
   QLabel* lb_status = da_model->statlab();
   QString text;

   if ( stat == ACT_CANCELLED )
   {  // cancel was selected:  report it
      text = tr( "\"%1\" Cancelled!" ).arg( item_act );
   }

   else if ( stat == ACT_NOTHING )
   {  // nothing under the selection could take this action
      text = tr( "\"%1\": nothing to do -- no selected record"
                 " can take this action" ).arg( item_act );
      QMessageBox::information( parentw, item_act, text );
   }

   else if ( stat == ACT_REFUSED )
   {  // the GMP guard said no, and has said why
      text = tr( "\"%1\" refused: GMP data" ).arg( item_act );
   }

   else if ( stat == 0 )
   {  // action was successful
      text = tr( "\"%1\" Success!" ).arg( item_act );
   }

   else
   {  // action got an error
      QMessageBox::warning( parentw,
            item_act + tr( " *ERROR*!" ),
            tr( "The \"%1\" action had an error: %2" )
            .arg( item_act ).arg( stat )
            + "\n\n" + ( da_process ? da_process->lastError() : QString() ) );
      text = tr( "\"%1\" ERROR!" ).arg( item_act );
   }

   if ( lb_status != NULL )
      lb_status->setText( text );
}

// compose string describing record state
QString US_DataTree::record_state( int istate )
{
   QString hexn    = QString::asprintf( "0x%3.3x", istate );

   QString flags   = "NOSTAT";         // by default, no state

   if ( istate & US_DataModel::REC_DB )
      flags  = flags + "|REC_DB";      // record exists in db

   if ( istate & US_DataModel::REC_LO )
      flags  = flags + "|REC_LO";      // record exists locally

   if ( istate & US_DataModel::PAR_DB )
      flags  = flags + "|PAR_DB";      // parent exists in db

   if ( istate & US_DataModel::PAR_LO )
      flags  = flags + "|PAR_LO";      // parent exists locally

   if ( istate & US_DataModel::HV_DET )
      flags  = flags + "|VERIFIED";    // the two copies were compared

   if ( istate & US_DataModel::IS_CON )
      flags  = flags + "|CONSIS";      // record is consistent in db+local

   if ( istate & US_DataModel::ALL_OK )
      flags  = flags + "|ALL_OK";      // record is ok in all respects

   if ( flags != "NOSTAT" )
      flags  = flags.mid( 7, 999 );    // remove any "NOSTAT|"

    return "(" + hexn + ") " + flags;  // return hex flag and text version
}

// The model IDs in a list of autoflow model descriptions
QStringList US_DataTree::parse_models_desc_json( const QStringList& modelInfos )
{
   QStringList modelIDs;

   for ( int ii = 0; ii < modelInfos.size(); ii++ )
   {
      const QString& modelDescJson = modelInfos[ ii ];

      if ( modelDescJson.isEmpty() )   continue;

      QJsonObject json_obj = QJsonDocument::fromJson( modelDescJson.toUtf8() )
                             .object();
      QStringList keys     = json_obj.keys();

      for ( int jj = 0; jj < keys.size(); jj++ )
      {
         const QString& key = keys[ jj ];

         if ( key != "2DSA_IT"  &&  key != "2DSA_MC"  &&  key != "PCSA" )
            continue;

         QJsonArray json_array = json_obj.value( key ).toArray();

         for ( int kk = 0; kk < json_array.size(); kk++ )
         {
            QJsonValue mid = json_array[ kk ].toObject().value( "modelID" );

            if ( ! mid.isUndefined() )
               modelIDs << mid.toVariant().toString();
         }
      }
   }

   return modelIDs;
}

// True when a model was produced by an autoflow (GMP) run
//
// The list is read once per action, over the connection the window already
// has, rather than a connection per record.
bool US_DataTree::is_modelIDs_from_autoflow( const QString& mID )
{
   if ( ! gmp_models_read )
   {
      gmp_models_read = true;
      gmp_models.clear();

      IUS_DB2* dbP = da_model->dbase();

      if ( dbP != NULL )
      {
         QStringList infos;
         QStringList q;
         q << "get_modelIDs_for_autoflow";
         dbP->query( q );

         while ( dbP->next() )
            infos << dbP->value( 0 ).toString();

         gmp_models = parse_models_desc_json( infos );
      }
   }

   return gmp_models.contains( mID );
}

// True when the run is part of the GMP framework
//
// Asked once per run per action, over the window's own connection.
bool US_DataTree::check_filename_for_autoflow( const QString& runID )
{
   if ( gmp_runs.contains( runID ) )
      return gmp_runs.value( runID );

   bool     isRequired = false;
   IUS_DB2* dbP        = da_model->dbase();

   if ( dbP != NULL  &&  ! runID.isEmpty() )
   {
      QStringList q;
      q  << QString( "check_filename_for_autoflow" ) //for now, both GMP and R&D
         << runID;

      isRequired = ( dbP->functionQuery( q ) > 0 );
   }

DbgLv(1) << "check_filename_for_autoflow: runID isRequired" << runID << isRequired;
   gmp_runs.insert( runID, isRequired );

   return isRequired;
}

// Act on the selection the tree has now
int US_DataTree::act_on_selection( QString item_exs, QString item_act )
{
   selitems = tw_recs->selectedItems();

   if ( selitems.isEmpty() )   return ACT_NOTHING;

   tw_item  = selitems.first();

   return do_actions( item_exs, item_act );
}

// The rows the tree's selection acts on
QVector< int > US_DataTree::action_row_list( void )
{
   selitems = tw_recs->selectedItems();
   action_rows();

   return actrows;
}

// Perform the action(s) chosen in context menu
int US_DataTree::do_actions( QString item_exs, QString item_act )
{
   if ( da_process == NULL )
      da_process = (US_DataProcess*)da_model->procobj();

   int narows  = action_rows();
DbgLv(1) << "ITEM do_actions" << narows << item_exs << item_act;
   int naerrs  = 0;
   int nacted  = 0;
   int istat   = 0;
   bool frDB   = item_exs.contains( "Datab" )
              || item_exs.contains( "DB "   );
   bool frLoc  = item_exs.contains( "Local" );
   bool frBoth = item_exs.contains( "both"  );
   frDB        = frBoth ? true : frDB;
   frLoc       = frBoth ? true : frLoc;
   bool dnLoad = item_act.contains( "Local file " );
   bool upLoad = item_act.contains( "DB create"   )
              || item_act.contains( "DB replace"  );
   bool remove = item_act.contains( "remove"      );
   QProgressBar* progress = da_model->progrBar();
   QLabel*       stlabel  = da_model->statlab();

   gmp_runs.clear();
   gmp_models.clear();
   gmp_models_read = false;

   if ( remove && frDB )
   {  // Nothing is removed if any of it is required by the GMP framework
      for ( int jj = 0; jj < narows; jj++ )
      {
         int irow   = actrows[ jj ];
         US_DataModel::DataDesc ddesc = da_model->row_datadesc( irow );

         if ( ( ddesc.recState & US_DataModel::REC_DB ) == 0 )   continue;

         QString runID = da_model->run_id_of_row( irow );

         if ( check_filename_for_autoflow( runID ) )
         {
            QMessageBox::information( parentw, tr( "Data Cannot be Deleted" ),
               tr( "The Data for the Run:\n\n"
                   "\"%1\"\n\n"
                   "can NOT be deleted since it is required by the GMP framework!" )
               .arg( runID ) );
            return ACT_REFUSED;
         }

         if ( ddesc.recType == US_DataModel::MODEL  ||
              ddesc.recType == US_DataModel::NOISE )
         {
            int model_idDB = ( ddesc.recType == US_DataModel::MODEL )
                           ? ddesc.recordID : ddesc.parentID;

            if ( is_modelIDs_from_autoflow( QString::number( model_idDB ) ) )
            {
               QMessageBox::information( parentw,
                  tr( "Selected Model GMP produced!" ),
                  tr( "Selected Model / Noise "
                      "was generated within GMP framework!\n\n"
                      "It can NOT be deleted..." ) );
               return ACT_REFUSED;
            }
         }
      }
   }

   if ( remove && frDB )
   {                                  // REMOVE from DB
      // The database removes the descendants of a record with it, so a
      // descendant of a record just removed is only marked as gone
      QSet< int > cascaded;

      for ( int jj = 0; jj < narows; jj++ )
      {
         int irow   = actrows[ jj ];
         US_DataModel::DataDesc ddesc = da_model->row_datadesc( irow );

         if ( ( ddesc.recState & US_DataModel::REC_DB ) == 0 )   continue;

         if ( cascaded.contains( irow ) )
         {
            ddesc.recordID  = -1;
            ddesc.recState &= ~( US_DataModel::REC_DB | US_DataModel::HV_DET );

            if ( ( ddesc.recState & US_DataModel::REC_LO ) == 0 )
               ddesc.recState  = US_DataModel::NOSTAT;

            da_model->change_datadesc( ddesc, irow );
            continue;
         }

         nacted++;
         int stat1  = da_process->record_remove_db( irow );
DbgLv(1) << "RMV_REC: irow stat1" << irow << stat1;

         if ( stat1 != 0 )
         {
            naerrs++;
            istat      = stat1;
         }

         else
         {
            QVector< int > drows = da_model->descendant_rows( irow );

            for ( int kk = 0; kk < drows.size(); kk++ )
               cascaded << drows[ kk ];
         }
      }
   }

   if ( remove && frLoc )
   {                                  // REMOVE from LOCAL
      for ( int ii = 4; ii > 1; ii-- )
      {  // Remove records from bottom up
         for ( int jj = 0; jj < narows; jj++ )
         {
            int irow   = actrows[ jj ];
            US_DataModel::DataDesc ddesc = da_model->row_datadesc( irow );

            if ( ddesc.recType != ii )  continue;  // not at current level
            if ( ( ddesc.recState & US_DataModel::REC_LO ) == 0 )  continue;

            nacted++;
            int stat1  = da_process->record_remove_local( irow );

            if ( stat1 != 0 )
            {
               naerrs++;
               istat      = stat1;
            }
         }
      }
   }

   if ( upLoad )
   {                                  // UPLOAD to DB
      for ( int ii = 2; ii < 5; ii++ )
      {  // Upload records from top down
         for ( int jj = 0; jj < narows; jj++ )
         {
            int irow   = actrows[ jj ];
            US_DataModel::DataDesc ddesc = da_model->row_datadesc( irow );

            if ( ddesc.recType != ii )  continue;  // not at current level
            if ( ( ddesc.recState & US_DataModel::REC_LO ) == 0 )  continue;

            nacted++;
            int stat1  = da_process->record_upload( irow );

            if ( stat1 != 0 )
            {
               naerrs++;
               istat      = stat1;
            }
         }
      }
   }

   if ( dnLoad )
   {                                  // DOWNLOAD to LOCAL
      if ( stlabel != NULL )
         stlabel->setText( tr( "Downloading records to local disk..." ) );

      if ( progress != NULL )
      {
         progress->setMaximum( qMax( narows, 1 ) );
         progress->setValue  ( 0 );
      }

      int ndown = 0;

      for ( int ii = 2; ii < 5; ii++ )
      {  // Download records from top down
         for ( int jj = 0; jj < narows; jj++ )
         {
            int irow   = actrows[ jj ];
            US_DataModel::DataDesc ddesc = da_model->row_datadesc( irow );

            if ( ddesc.recType != ii )  continue;  // not at current level
            if ( ( ddesc.recState & US_DataModel::REC_DB ) == 0 )  continue;

            nacted++;
            int stat1  = da_process->record_download( irow );

            if ( stat1 != 0 )
            {
               naerrs++;
               istat      = stat1;
            }

            if ( progress != NULL )
               progress->setValue( ++ndown );
         }
      }
   }

   // The rows acted on show where their records are now
   QList< int > runs;

   for ( int jj = 0; jj < narows; jj++ )
   {
      int runIndex = da_model->run_of_row( actrows[ jj ] );

      if ( runIndex >= 0  &&  ! runs.contains( runIndex ) )
         runs << runIndex;
   }

   for ( int jj = 0; jj < runs.size(); jj++ )
      refresh_run( runs[ jj ] );

   if ( nacted == 0 )
      return ACT_NOTHING;              // never "Success" for doing nothing

   if ( naerrs > 0  &&  naerrs < nacted )
   {  // Append to error message, explaining partial success
      da_process->partialError( naerrs, nacted );
   }

   return istat;
}

/* Create lists of selected rows and implied action rows.

   The rows below a row are not necessarily its descendants: the experiment
   rows come first and each experiment's records follow all of them, in the
   order the experiments were read.  So the descendants of a row are asked
   of the model, which knows each experiment's row range.
*/
int US_DataTree::action_rows( )
{
   int nsrows = selitems.size();
   selrows.clear();
   actrows.clear();
   rawrows.clear();

   for ( int ii = 0; ii < nsrows; ii++ )
   {
      int row = selitems[ ii ]->type() - (int)QTreeWidgetItem::UserType;

      if ( ! selrows.contains( row ) )
         selrows << row;
   }

   std::sort( selrows.begin(), selrows.end() );
   nsrows = selrows.size();

   QSet< int > acts;

   for ( int ii = 0; ii < nsrows; ii++ )
   {
      int irow   = selrows[ ii ];

      if ( irow < 0  ||  irow >= da_model->recCount() )   continue;

      int ityp   = da_model->row_datadesc( irow ).recType;
DbgLv(1) << "acrow:  irow ityp" << irow << ityp;

      if ( ityp > US_DataModel::RAW )
         acts << irow;        // add to action list if not Raw
      else if ( ityp == US_DataModel::RAW  &&  ! rawrows.contains( irow ) )
         rawrows << irow;     // otherwise, add to Raw-selected list

      QVector< int > drows = da_model->descendant_rows( irow );

      for ( int jj = 0; jj < drows.size(); jj++ )
      {
         int jrow = drows[ jj ];

         if ( da_model->row_datadesc( jrow ).recType <= US_DataModel::RAW )
         {  // a Raw under a selected experiment is not acted on itself
            if ( ! rawrows.contains( jrow ) )
               rawrows << jrow;
            continue;
         }

         acts << jrow;
      }
   }

   actrows = QVector< int >( acts.begin(), acts.end() );
   std::sort( actrows.begin(), actrows.end() );   // tree order
DbgLv(1) << "acrow:  narows nrrows" << actrows.size() << rawrows.size();

   return actrows.size();
}

// Check model or noise for significant differences
bool US_DataTree::significant_diffs( US_DataModel::DataDesc ddesc,
      IUS_DB2* dbP )
{
   bool differs = true;   // Assume significant differences unless model,noise
   const double dtoler = 5.0e-5;

   // If model, check for differences beyond tolerance in component values
   if ( ddesc.recType == 3 )
   {
      US_Model dmodel;
      US_Model fmodel;
      double   difmax = 0.0;

      int dst = dmodel.load( QString::number( ddesc.recordID ), dbP );
      int fst = fmodel.load( ddesc.filename );

      if ( dst != US_DB2::OK )
         dmodel.description = "DBAD:" + dmodel.description;

      if ( fst != US_DB2::OK )
         fmodel.description = "FBAD:" + fmodel.description;

DbgLv(1) << " CONFLICT? dst fst differs" << dst << fst << differs;
      if ( dmodel == fmodel )
         differs = false;          // Differences only in formatting

      else if ( ( dmodel.monteCarlo    == fmodel.monteCarlo    ) &&
                ( dmodel.description   == fmodel.description   ) &&
                ( dmodel.modelGUID     == fmodel.modelGUID     ) &&
                ( dmodel.editGUID      == fmodel.editGUID      ) &&
                ( dmodel.wavelength    == fmodel.wavelength    ) &&
                ( dmodel.variance      == fmodel.variance      ) &&
                ( dmodel.meniscus      == fmodel.meniscus      ) &&
                ( dmodel.optics        == fmodel.optics        ) &&
                ( dmodel.analysis      == fmodel.analysis      ) &&
                ( dmodel.global        == fmodel.global        ) &&
                ( dmodel.coSedSolute   == fmodel.coSedSolute   ) &&
                ( dmodel.components.size() == fmodel.components.size() ) )
      {  // Difference can only be in component values
         for ( int ii = 0; ii < dmodel.components.size(); ii++ )
         {
            if ( dmodel.components[ ii ] != fmodel.components[ ii ] )
            {  // There is a components difference, so measure it
               US_Model::SimulationComponent* dcomp = &dmodel.components[ ii ];
               US_Model::SimulationComponent* fcomp = &fmodel.components[ ii ];
               double dcvalu = qAbs( dcomp->signal_concentration );
               double fcvalu = qAbs( fcomp->signal_concentration );
               double valmin = qMin( dcvalu, fcvalu );
                      valmin = ( valmin == 0.0 ) ? dtoler : valmin;
               double difrat = qAbs( dcvalu - fcvalu ) / valmin;
                      difmax = qMax( difrat, difmax );

               dcvalu = qAbs( dcomp->molar_concentration );
               fcvalu = qAbs( fcomp->molar_concentration );
               valmin = qMin( dcvalu, fcvalu );
               valmin = ( valmin == 0.0 ) ? dtoler : valmin;
               difrat = qAbs( dcvalu - fcvalu ) / valmin;
               difmax = qMax( difrat, difmax );

               dcvalu = qAbs( dcomp->vbar20 );
               fcvalu = qAbs( fcomp->vbar20 );
               valmin = qMin( dcvalu, fcvalu );
               valmin = ( valmin == 0.0 ) ? dtoler : valmin;
               difrat = qAbs( dcvalu - fcvalu ) / valmin;
               difmax = qMax( difrat, difmax );

               dcvalu = qAbs( dcomp->mw );
               fcvalu = qAbs( fcomp->mw );
               valmin = qMin( dcvalu, fcvalu );
               valmin = ( valmin == 0.0 ) ? dtoler : valmin;
               difrat = qAbs( dcvalu - fcvalu ) / valmin;
               difmax = qMax( difrat, difmax );

               dcvalu = qAbs( dcomp->s );
               fcvalu = qAbs( fcomp->s );
               valmin = qMin( dcvalu, fcvalu );
               valmin = ( valmin == 0.0 ) ? dtoler : valmin;
               difrat = qAbs( dcvalu - fcvalu ) / valmin;
               difmax = qMax( difrat, difmax );

               dcvalu = qAbs( dcomp->D );
               fcvalu = qAbs( fcomp->D );
               valmin = qMin( dcvalu, fcvalu );
               valmin = ( valmin == 0.0 ) ? dtoler : valmin;
               difrat = qAbs( dcvalu - fcvalu ) / valmin;
               difmax = qMax( difrat, difmax );
DbgLv(1) << "   CONFLICT? D dcv fcv dr" << dcvalu << fcvalu << difrat;

               dcvalu = qAbs( dcomp->f_f0 );
               fcvalu = qAbs( fcomp->f_f0 );
               valmin = qMin( dcvalu, fcvalu );
               valmin = ( valmin == 0.0 ) ? dtoler : valmin;
               difrat = qAbs( dcvalu - fcvalu ) / valmin;
               difmax = qMax( difrat, difmax );
            }
         }

         differs = ( difmax > dtoler );
DbgLv(1) << " CONFLICT? model difmax differs" << difmax << differs;
      }

   }

   // if noise, check for differences beyond tolerance in noise values
   else if ( ddesc.recType == 4)
   {
      US_Noise dnoise;
      US_Noise fnoise;
      double   difmax = 0.0;

      int dst = dnoise.load( QString::number( ddesc.recordID ), dbP );
      int fst = fnoise.load( ddesc.filename );

      if ( dst != US_DB2::OK )
         dnoise.description = "DBAD:" + dnoise.description;

      if ( fst != US_DB2::OK )
         fnoise.description = "FBAD:" + fnoise.description;

      if ( dnoise == fnoise )
         differs = false;          // Differences only in formatting

      else if ( ( dnoise.type          == fnoise.type          ) &&
                ( dnoise.description   == fnoise.description   ) &&
                ( dnoise.noiseGUID     == fnoise.noiseGUID     ) &&
                ( dnoise.modelGUID     == fnoise.modelGUID     ) &&
                ( dnoise.minradius     == fnoise.minradius     ) &&
                ( dnoise.maxradius     == fnoise.maxradius     ) &&
                ( dnoise.values.size() == fnoise.values.size() ) )
      {  // Difference can only be in noise values
         for ( int ii = 0; ii < dnoise.values.size(); ii++ )
         {
            double dnvalu = qAbs( dnoise.values[ ii ] );
            double fnvalu = qAbs( fnoise.values[ ii ] );
            double valmin = qMin( dnvalu, fnvalu );
                   valmin = ( valmin == 0.0 ) ? dtoler : valmin;
            double difrat = qAbs( dnvalu - fnvalu ) / valmin;
                   difmax = qMax( difrat, difmax );
         }

         differs = ( difmax > dtoler );
DbgLv(1) << " CONFLICT? model difmax differs" << difmax << differs;
      }
   }

   return differs;
}

