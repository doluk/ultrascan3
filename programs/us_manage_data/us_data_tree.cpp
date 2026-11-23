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
      QWidget* parent /*=0*/ ) : QObject( parent )
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
         // values were written out.  Asked once per record and comparison:
         // it reads the record from both places.
         if ( ( ityp == US_DataModel::MODEL  ||  ityp == US_DataModel::NOISE )
              &&  da_model->dbase() != NULL  &&  desc.recordID >= 0
              &&  ! da_model->is_demo() )
         {
            QString key = desc.dataGUID + "|" + desc.contents;

            if ( ! sigdiffs.contains( key ) )
               sigdiffs.insert( key,
                                significant_diffs( desc, da_model->dbase() ) );

            if ( ! sigdiffs.value( key ) )
            {
               fbru = QBrush( colorGreen );
               rsrc = "In Sync";
            }
         }
         break;

      case US_DataModel::SYNC_DUMMY:
      default:
         fbru    = QBrush( colorGray );
         rsrc    = "dummy";
         break;
   }
}

// The look of an experiment row
//
// An experiment in both places says what its records say, as the tree shows
// them: in conflict when one of them is, unverified while one of them is
// not compared yet, and in sync otherwise.  A stand-in experiment is no
// record of either place, and looks neutral.
void US_DataTree::run_look( int runIndex, QBrush& fbru, QString& rtyp,
                            QString& rsrc )
{
   US_DataModel::RunEntry entry = da_model->run_entry( runIndex );
   US_DataModel::DataDesc desc  = da_model->row_datadesc( entry.row );

   row_look( desc, fbru, rtyp, rsrc );

   if ( entry.orphans )
   {
      fbru = QBrush( colorGray );
      rsrc = "--";
      return;
   }

   US_DataModel::SyncState state = US_DataModel::sync_state( desc );

   if ( state != US_DataModel::SYNC_UNVERIFIED  &&
        state != US_DataModel::SYNC_INSYNC      &&
        state != US_DataModel::SYNC_CONFLICT )
      return;                          // in one place only

   if ( ! entry.loaded )               // nothing under it to go by yet
      return;

   bool unverified = ! entry.verified;
   bool conflict   = false;

   for ( int row = entry.firstRow; row >= 0  &&  row <= entry.lastRow; row++ )
   {
      US_DataModel::DataDesc cdsc = da_model->row_datadesc( row );

      if ( ( cdsc.recState & US_DataModel::REC_DB ) == 0  ||
           ( cdsc.recState & US_DataModel::REC_LO ) == 0 )
         continue;

      QBrush  cbru;
      QString ctyp;
      QString csrc;
      row_look( cdsc, cbru, ctyp, csrc );

      if ( csrc == "Conflict"   )   conflict   = true;
      if ( csrc == "Unverified" )   unverified = true;
   }

   if ( conflict )
   {
      fbru = QBrush( colorRed );
      rsrc = "Conflict";
   }

   else if ( unverified )
   {
      fbru = QBrush( colorTeal );
      rsrc = "Unverified";
   }

   else
   {
      fbru = QBrush( colorGreen );
      rsrc = "In Sync";
   }
}

// The prompter that shows message boxes
bool US_DataTree::ask_user( int kind, const QString& title,
                            const QString& text )
{
   if ( prompter )
      return prompter( kind, title, text );

   if ( kind == PROMPT_INFORM )
   {
      QMessageBox::information( parentw, title, text );
      return true;
   }

   return ( QMessageBox::question( parentw, title, text,
                                   QMessageBox::Yes | QMessageBox::No,
                                   QMessageBox::No ) == QMessageBox::Yes );
}

void US_DataTree::setPrompter(
      std::function< bool( int, const QString&, const QString& ) > func )
{
   prompter = func;
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
   sigdiffs.clear();

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
   run_look( runIndex, fbru, rtyp, rsrc );

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

   else if ( entry.globals )
   {
      for ( int jj = 0; jj < ntcols; jj++ )
         item->setToolTip( jj, tr( "Global models, which belong to no edit"
                                   " by design, and their noise; the gray"
                                   " rows stand in for the edit and raw"
                                   " data they do not have." ) );
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
      QString anch;
      QString ande;
      row_counts( row, entry.lastRow, anch, ande );

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

   refresh_run_item( runIndex );
}

// The "Children" and "Descendants" texts of one record row
void US_DataTree::row_counts( int row, int lastRow, QString& anch,
                              QString& ande )
{
   int ityp  = da_model->row_datadesc( row ).recType;
   int kchdb = 0;
   int kchlo = 0;
   int kdedb = 0;
   int kdelo = 0;

   for ( int jj = row + 1; jj <= lastRow; jj++ )
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

   anch = ( ityp < 4 ) ? QString( "%1, %2" ).arg( kchdb ).arg( kchlo ) : "";
   ande = ( ityp < 3 ) ? QString( "%1, %2" ).arg( kdedb ).arg( kdelo ) : "";
}

// Show an experiment's records again, from the rows it was merged into anew
//
// The items of its records go, and they may be selected, or be the item a
// context menu is being opened for: each is replaced there by the new item
// of the same record.
void US_DataTree::refill_run( int runIndex )
{
   if ( runIndex < 0  ||  runIndex >= runitems.size() )   return;

   QTreeWidgetItem* runitem = runitems[ runIndex ];

   QList< QTreeWidgetItem* > olds;
   QList< QTreeWidgetItem* > stack;

   for ( int ii = 0; ii < runitem->childCount(); ii++ )
      stack << runitem->child( ii );

   while ( ! stack.isEmpty() )
   {
      QTreeWidgetItem* item = stack.takeFirst();
      olds << item;

      for ( int ii = 0; ii < item->childCount(); ii++ )
         stack << item->child( ii );
   }

   QSet< QString > selkeys;          // selected in the tree
   QSet< QString > actkeys;          // in the selection an action uses
   QString         curkey;
   bool            curgone = false;

   for ( int ii = 0; ii < olds.size(); ii++ )
   {
      QTreeWidgetItem* item = olds[ ii ];
      QString          key  = item_key( item );

      if ( item->isSelected() )         selkeys << key;
      if ( selitems.contains( item ) )  actkeys << key;

      if ( item == tw_item )
      {
         curkey  = key;
         curgone = true;
      }

      selitems.removeAll( item );
   }

   if ( curgone )
      tw_item = NULL;

   qDeleteAll( runitem->takeChildren() );

   if ( da_model->run_entry( runIndex ).loaded )
      fill_run( runIndex );
   else
      refresh_run_item( runIndex );

   // The same records, in their new items
   for ( int ii = 0; ii < runitem->childCount(); ii++ )
      stack << runitem->child( ii );

   while ( ! stack.isEmpty() )
   {
      QTreeWidgetItem* item = stack.takeFirst();
      QString          key  = item_key( item );

      for ( int ii = 0; ii < item->childCount(); ii++ )
         stack << item->child( ii );

      if ( selkeys.contains( key ) )
         item->setSelected( true );

      if ( actkeys.contains( key )  &&  ! selitems.contains( item ) )
         selitems << item;

      if ( curgone  &&  tw_item == NULL  &&  key == curkey )
         tw_item = item;
   }
}

// What a record's item is found again by: its type and GUID, or for a
// filler, which has no GUID, its label
QString US_DataTree::item_key( QTreeWidgetItem* item )
{
   int row = item->type() - (int)QTreeWidgetItem::UserType;

   if ( row < 0  ||  row >= da_model->recCount() )   return QString();

   US_DataModel::DataDesc desc = da_model->row_datadesc( row );
   QString guid = desc.dataGUID.simplified().toLower();

   return QString::number( desc.recType ) + ":"
          + ( guid.isEmpty() ? "~" + desc.label : guid );
}

// Colour an experiment row from what is under it now, and show its counts
void US_DataTree::refresh_run_item( int runIndex )
{
   if ( runIndex < 0  ||  runIndex >= runitems.size() )   return;

   QBrush  fbru( colorBrown );
   QString rtyp;
   QString rsrc;
   run_look( runIndex, fbru, rtyp, rsrc );

   QTreeWidgetItem* item = runitems[ runIndex ];
   item->setText( 3, rsrc );

   for ( int jj = 0; jj < ntcols; jj++ )
      item->setForeground( jj, fbru );

   US_DataModel::RunEntry entry = da_model->run_entry( runIndex );

   if ( ! entry.loaded )   return;           // the listing's counts stand

   // the counts on the experiment row are what is really under it -- the
   // model counted the descendants the same way; the raws are its children
   int nchdb = 0;
   int nchlo = 0;

   for ( int row = entry.firstRow; row >= 0  &&  row <= entry.lastRow; row++ )
   {
      US_DataModel::DataDesc desc = da_model->row_datadesc( row );

      if ( desc.recType != US_DataModel::RAW )   continue;

      nchdb  += ( ( desc.recState & US_DataModel::REC_DB ) != 0 ) ? 1 : 0;
      nchlo  += ( ( desc.recState & US_DataModel::REC_LO ) != 0 ) ? 1 : 0;
   }

   item->setText( 4, QString( "%1, %2" ).arg( nchdb ).arg( nchlo ) );
   item->setText( 5, QString( "%1, %2" ).arg( entry.dbCount )
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

   for ( int ii = 0; ii < runitems[ runIndex ]->childCount(); ii++ )
      stack << runitems[ runIndex ]->child( ii );

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
      QString anch;
      QString ande;

      row_look  ( desc, fbru, rtyp, rsrc );
      row_counts( row, entry.lastRow, anch, ande );

      item->setText( 3, rsrc );
      item->setText( 4, anch );
      item->setText( 5, ande );

      for ( int jj = 0; jj < ntcols; jj++ )
         item->setForeground( jj, fbru );
   }

   refresh_run_item( runIndex );
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

// Take the selection a context menu acts on
bool US_DataTree::context_selection( QTreeWidgetItem* item )
{
   if ( item == NULL )                                     return false;
   if ( da_model->scan_active()  ||  da_model->is_busy() ) return false;

   // An experiment acts on its records, so they have to be read.  Reading
   // one can merge another experiment again, whose items are then new ones
   // (refill_run): what the items are is taken first, and the item and the
   // selection only after the reads.
   QList< QTreeWidgetItem* > sel = tw_recs->selectedItems();
   QList< int >              runs;

   if ( ! sel.contains( item ) )
      sel << item;

   for ( int ii = 0; ii < sel.size(); ii++ )
   {
      int runIndex = ( sel[ ii ]->parent() == NULL )
                   ? runitems.indexOf( sel[ ii ] ) : -1;

      if ( runIndex >= 0  &&  ! runs.contains( runIndex ) )
         runs << runIndex;
   }

   tw_item  = item;
   selitems = sel;

   for ( int ii = 0; ii < runs.size(); ii++ )
      ensure_loaded( runs[ ii ] );

   if ( tw_item == NULL )            // its record is no longer shown
      return false;

   selitems = tw_recs->selectedItems();

   if ( ! selitems.contains( tw_item ) )
      selitems << tw_item;

   return true;
}

QTreeWidgetItem* US_DataTree::current_item( void ) const
{
   return tw_item;
}

// set up and display a row context menu
void US_DataTree::row_context_menu( QTreeWidgetItem* item )
{
   if ( ! context_selection( item ) )                      return;

   item     = tw_item;
   int nsel = selitems.size();
DbgLv(1) << "    context_menu nbr sel rows" << selitems.size();
   int irow = item->type() - (int)QTreeWidgetItem::UserType;

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

   // The sample tree is no data anywhere: it can only be looked at
   if ( da_model->is_demo() )
   {
      aupload->setEnabled( false );
      adnload->setEnabled( false );
      adbasrm->setEnabled( false );
      aloclrm->setEnabled( false );
      abothrm->setEnabled( false );
   }

   // display the context menu and act on selection
   QAction* chosen = cmenu.exec( QCursor::pos() );

   // While the menu was up the window ran its background reads, which can
   // show an experiment anew: the item is then the new one, or gone
   if      ( chosen == NULL )      return;
   else if ( tw_item == NULL )     return;
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
      tr( "              such fillers, under an experiment row of their\n" ) +
      tr( "              own, \"(records of no experiment)\", so they can\n" ) +
      tr( "              still be removed -- all but raw data, which the\n" ) +
      tr( "              database removes only with its experiment, and\n" ) +
      tr( "              whose experiment is missing or not yours.  Global\n" ) +
      tr( "              models, which have no edit by design, are under\n" ) +
      tr( "              \"(global models)\".\n" ) +
      tr( "              These two rows are no record themselves, and\n" ) +
      tr( "              their Source reads \"--\"; removing from one of\n" ) +
      tr( "              them asks first, saying how many records go.\n" ) +
      tr( "              A record whose parent is only in the other place\n" ) +
      tr( "              is shown under that parent, paired with its\n" ) +
      tr( "              other copy.\n\n" ) +
      tr( "  An experiment row in both places reads Conflict when any\n" ) +
      tr( "  record under it does, Unverified while any is not compared,\n" ) +
      tr( "  and In Sync otherwise.  A run marked \"(!)\" has a problem;\n" ) +
      tr( "  hold the mouse over it to see which.  Run IDs with dots are\n" ) +
      tr( "  legacy: still shown, no longer accepted for new runs.  When\n" ) +
      tr( "  its local directory is not named after it, or is not the only\n" ) +
      tr( "  one holding it, nothing is uploaded or downloaded for it; its\n" ) +
      tr( "  records can still be removed.\n\n" ) +
      tr( "Scan Progress --\n\n" ) +
      tr( "  The upper bar counts the steps of the phase that is running\n" ) +
      tr( "  (listing, reading, comparing); the lower bar the work inside\n" ) +
      tr( "  the experiment being read.  The window takes input between\n" ) +
      tr( "  experiments; a database query holds it up while it runs.\n" ) +
      tr( "  While the whole local store is read it is redrawn but takes\n" ) +
      tr( "  no input; with a run selected, that run's local files are\n" ) +
      tr( "  read in the window itself, which is not redrawn until then.\n" ) +
      tr( "  Opening an experiment while a scan runs moves it to the front\n" ) +
      tr( "  of the queue.  Closing the window stops the scan.\n\n" ) +
      tr( "Tree Entry Processes --\n\n" ) +
      tr( "  A right-mouse-button click on any cell of a row pops up\n" ) +
      tr( "  a context menu with actions to take for the record.\n" ) +
      tr( "  Menu items will only be enabled if the action is\n" ) +
      tr( "  appropriate for the particular record shown in the row.\n" ) +
      tr( "  On an experiment or a raw record, an action applies to\n" ) +
      tr( "  everything under it.  No action is offered while a scan runs,\n" ) +
      tr( "  nor on the sample tree shown before the first scan.\n" ) +
      tr( "  Possible actions are:\n" ) +
      tr( "    \"upload to DB\"\n" ) +
      tr( "       -> Upload this local record to the database.  A record\n" ) +
      tr( "          the database holds already is replaced only when the\n" ) +
      tr( "          two copies differ, and then not by an older copy.\n" ) +
      tr( "    \"download to local\"\n" ) +
      tr( "       -> Download this DB record to local disk; its parent\n" ) +
      tr( "          has to be on local disk, or be downloaded with it.\n" ) +
      tr( "  A record changed by an action is compared again on its own.\n" ) +

      tr( "    \"remove from DB\"\n" ) +
      tr( "       -> Remove this record from the database.  Removing an\n" ) +
      tr( "          edit also removes the noise of another edit's\n" ) +
      tr( "          model that was computed from it (the GMP checks\n" ) +
      tr( "          apply to it too); an older database keeps the edit\n" ) +
      tr( "          while that noise refers to it, and your own such\n" ) +
      tr( "          noise is then removed first.  It is all done in\n" ) +
      tr( "          transactions.  On an older database another\n" ) +
      tr( "          investigator's noise keeps the edit; then nothing\n" ) +
      tr( "          is removed, and the message says why.  After a\n" ) +
      tr( "          failure what the database holds is read back, and\n" ) +
      tr( "          the rows show it.\n" ) +
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
   if ( tw_item == NULL )   return;

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

   // The background reads run while the question is asked, too: when the
   // item it was asked about went, nothing is done
   if ( msgBox.exec() == QMessageBox::Yes  &&  tw_item != NULL )
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
   {  // refused, and the user has been told why
      text = tr( "\"%1\" refused" ).arg( item_act );
   }

   else if ( stat == ACT_IN_SYNC )
   {  // a DB replace found the database holds what the files hold
      text = tr( "\"%1\": nothing to upload -- already in sync" )
             .arg( item_act );
      ask_user( PROMPT_INFORM, item_act, text );
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

// The GMP guard for the noise of other edits' models an edit removal takes
// along.  What cannot be listed is left to the removal, which reports it.
bool US_DataTree::foreign_noise_gmp_ok( const US_DataModel::DataDesc& ddesc,
                                        const QString& runID )
{
   if ( da_process == NULL )
      da_process = (US_DataProcess*)da_model->procobj();

   if ( ddesc.recordID < 0  ||  da_process == NULL )   return true;

   QList< US_DataProcess::ForeignNoise > foreign;

   if ( ! da_process->foreign_noise_of_edit( ddesc.recordID, foreign ) )
      return true;

   for ( int ii = 0; ii < foreign.size(); ii++ )
   {
      const US_DataProcess::ForeignNoise& fn = foreign.at( ii );
      QString name = fn.description.isEmpty() ? fn.guid : fn.description;

      if ( is_modelIDs_from_autoflow( QString::number( fn.modelID ) ) )
      {
         ask_user( PROMPT_INFORM, tr( "Selected Model GMP produced!" ),
            tr( "Removing %1 would also remove noise %2, which was computed"
                " from it for a model generated within the GMP framework."
                "\n\nIt can NOT be deleted..." )
            .arg( ddesc.label ).arg( name ) );
         return false;
      }

      // The run of that noise: where the tree shows it, else the one its
      // description names
      QString nrun;

      for ( int row = 0; row < da_model->recCount()  &&  nrun.isEmpty(); row++ )
      {
         US_DataModel::DataDesc ndesc = da_model->row_datadesc( row );

         if ( ndesc.recType == US_DataModel::NOISE  &&
              ndesc.recordID == fn.noiseID  &&
              da_model->run_of_row( row ) >= 0 )
            nrun = da_model->run_id_of_row( row );
      }

      if ( nrun.isEmpty() )
         nrun = ( fn.description.count( "." ) >= 3 )
              ? fn.description.section( ".", 0, -4 )
              : fn.description.section( ".", 0, 0 );

      if ( nrun != runID  &&  check_filename_for_autoflow( nrun ) )
      {
         ask_user( PROMPT_INFORM, tr( "Data Cannot be Deleted" ),
            tr( "Removing %1 would also remove noise %2 of the Run:\n\n"
                "\"%3\"\n\n"
                "which can NOT be deleted since it is required by the GMP"
                " framework!" ).arg( ddesc.label ).arg( name ).arg( nrun ) );
         return false;
      }
   }

   return true;
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

// A batch of record changes, ended when it goes out of scope at the latest
US_DataTree::ChangeBatch::ChangeBatch( US_DataModel* model ) : da_model( model )
{
   da_model->begin_changes();
}

US_DataTree::ChangeBatch::~ChangeBatch()
{
   end();
}

QList< int > US_DataTree::ChangeBatch::end( void )
{
   if ( da_model == NULL )   return QList< int >();

   QList< int > runs = da_model->end_changes();
   da_model = NULL;
   return runs;
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
   int nsynced = 0;
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

   // The sample tree shown before a scan is no data anywhere
   if ( da_model->is_demo() )
   {
      ask_user( PROMPT_INFORM, tr( "Sample Data" ),
                tr( "This tree is a sample of the states records can be"
                    " in, not your data.  Click \"Scan Data\" to see your"
                    " data and act on it." ) );
      return ACT_REFUSED;
   }

   // Nothing is written for a run whose directory is not where it should
   // be: an upload or a download would put its records somewhere else
   if ( upLoad || dnLoad )
   {
      QStringList unsafe;

      for ( int jj = 0; jj < narows; jj++ )
      {
         US_DataModel::RunEntry entry =
            da_model->run_entry( da_model->run_of_row( actrows[ jj ] ) );

         if ( entry.unsafe  &&  ! unsafe.contains( entry.runID ) )
            unsafe << entry.runID;
      }

      if ( ! unsafe.isEmpty() )
      {
         ask_user( PROMPT_INFORM, tr( "Run Marked \"(!)\"" ),
            tr( "Nothing is uploaded or downloaded for these runs, since"
                " their local directories are not named after them or are"
                " not the only ones holding them:\n\n  %1\n\n"
                "Hold the mouse over a run marked \"(!)\" to see why.  Its"
                " records can still be removed." )
            .arg( unsafe.join( "\n  " ) ) );
         return ACT_REFUSED;
      }
   }

   // Removing from a stand-in experiment removes everything under it, and
   // that is a whole class of records: it is spelled out and asked again
   if ( remove )
   {
      for ( int ii = 0; ii < selitems.size(); ii++ )
      {
         if ( selitems[ ii ]->parent() != NULL )   continue;

         int runIndex = runitems.indexOf( selitems[ ii ] );
         US_DataModel::RunEntry entry = da_model->run_entry( runIndex );

         if ( ! entry.orphans )                    continue;

         int count = 0;

         for ( int jj = 0; jj < narows; jj++ )
         {
            int irow = actrows[ jj ];

            if ( da_model->run_of_row( irow ) != runIndex )   continue;

            int state = da_model->row_datadesc( irow ).recState;

            if ( ( frDB  &&  ( state & US_DataModel::REC_DB ) != 0 )  ||
                 ( frLoc &&  ( state & US_DataModel::REC_LO ) != 0 ) )
               count++;
         }

         if ( count == 0 )                         continue;

         QString where = ( frDB && frLoc ) ? tr( "the database and local disk" )
                       : frDB ? tr( "the database" ) : tr( "local disk" );

         if ( ! ask_user( PROMPT_CONFIRM, tr( "Remove All of %1" )
                          .arg( entry.runID ),
                tr( "This removes all %1 record(s) under \"%2\" from %3.\n\n"
                    "%4\n\nRemove all %1 of them?" )
                .arg( count ).arg( entry.runID ).arg( where )
                .arg( entry.globals
                      ? tr( "Global models are not broken records: they"
                            " belong to no edit by design." )
                      : tr( "These are records whose experiment or parent"
                            " is missing." ) ) ) )
            return ACT_CANCELLED;
      }
   }

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
            ask_user( PROMPT_INFORM, tr( "Data Cannot be Deleted" ),
               tr( "The Data for the Run:\n\n"
                   "\"%1\"\n\n"
                   "can NOT be deleted since it is required by the GMP framework!" )
               .arg( runID ) );
            return ACT_REFUSED;
         }

         // An edit takes along noise of other edits' models computed from
         // it: that noise is guarded as if it had been selected
         if ( ddesc.recType == US_DataModel::EDIT  &&
              ! foreign_noise_gmp_ok( ddesc, runID ) )
            return ACT_REFUSED;

         if ( ddesc.recType == US_DataModel::MODEL  ||
              ddesc.recType == US_DataModel::NOISE )
         {
            int model_idDB = ( ddesc.recType == US_DataModel::MODEL )
                           ? ddesc.recordID : ddesc.parentID;

            if ( is_modelIDs_from_autoflow( QString::number( model_idDB ) ) )
            {
               ask_user( PROMPT_INFORM,
                  tr( "Selected Model GMP produced!" ),
                  tr( "Selected Model / Noise "
                      "was generated within GMP framework!\n\n"
                      "It can NOT be deleted..." ) );
               return ACT_REFUSED;
            }
         }
      }
   }

   // What the actions change is brought up to date once per experiment,
   // when they are all done, not once per record
   ChangeBatch batch( da_model );

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
            da_process->mark_removed_from_db( irow );
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

            int stat1  = da_process->record_upload( irow );

            if ( stat1 == US_DataProcess::IN_SYNC )
            {  // nothing to do for this one, and that is no error
               nsynced++;
               continue;
            }

            nacted++;

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

   // The rows acted on show where their records are now, and so do the
   // rows of any other experiment an action reached: the noise of another
   // edit's model an edit removal takes along may be under another one
   QList< int > runs = batch.end();

   for ( int jj = 0; jj < narows; jj++ )
   {
      int runIndex = da_model->run_of_row( actrows[ jj ] );

      if ( runIndex >= 0  &&  ! runs.contains( runIndex ) )
         runs << runIndex;
   }

   for ( int jj = 0; jj < runs.size(); jj++ )
      refresh_run( runs[ jj ] );

   if ( nacted == 0 )                  // never "Success" for doing nothing
      return ( nsynced > 0 ) ? ACT_IN_SYNC : ACT_NOTHING;


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

