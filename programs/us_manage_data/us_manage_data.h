//! \file us_manage_data.h
#ifndef US_MANAGE_DATA_H
#define US_MANAGE_DATA_H

#include "us_extern.h"
#include "us_data_model.h"
#include "us_data_process.h"
#include "us_data_tree.h"
#include "us_sync_exper.h"
#include "us_widgets.h"
#include "us_db2.h"
#include "us_model.h"
#include "us_buffer.h"
#include "us_analyte.h"
#include "us_help.h"
#include "us_dataIO.h"
#include "us_settings.h"

//! \class US_ManageData
//! \brief Class to manage data including display, processing, and database interactions.
class US_ManageData : public US_Widgets
{
    Q_OBJECT

    public:
        //! \brief Constructor for US_ManageData
        US_ManageData();

        US_DB2*       db;               //!< Database connection

    private:
        QTreeWidget*        tw_recs;    //!< Tree widget
        US_DataModel*       da_model;   //!< Underlying data handler
        US_DataProcess*     da_process; //!< Data processing handler
        US_DataTree*        da_tree;    //!< Data tree display handler
        QProgressBar*       progress;   //!< Overall progress of a scan
        QProgressBar*       pgb_step;   //!< Progress within the current step
        US_Help             showHelp;   //!< Help display
        QLabel*             lb_status;  //!< Status label
        QTextEdit*          te_status;  //!< Status text edit
        QLineEdit*          le_invtor;  //!< Investigator line edit
        QPushButton*        pb_invtor;  //!< Investigator button
        QPushButton*        pb_reset;   //!< Reset button
        QPushButton*        pb_scanda;  //!< Scan data button
        QPushButton*        pb_hsedit;  //!< Hide/show edits button
        QPushButton*        pb_hsmodl;  //!< Hide/show models button
        QPushButton*        pb_hsnois;  //!< Hide/show noises button
        QPushButton*        pb_helpdt;  //!< Data help button
        QPushButton*        pb_help;    //!< Help button
        QPushButton*        pb_close;   //!< Close button
        QComboBox*          cb_runid;   //!< Run ID combo box
        QComboBox*          cb_triple;  //!< Triple combo box
        QComboBox*          cb_source;  //!< Source combo box

        int                 personID;   //!< Person ID
        int                 ntcols;     //!< Number of tree columns
        int                 ncrecs;     //!< Number of current records
        int                 ncraws;     //!< Number of current raw records
        int                 ncedts;     //!< Number of current edit records
        int                 ncmods;     //!< Number of current model records
        int                 ncnois;     //!< Number of current noise records
        int                 ndrecs;     //!< Number of database records
        int                 ndraws;     //!< Number of database raw records
        int                 ndedts;     //!< Number of database edit records
        int                 ndmods;     //!< Number of database model records
        int                 ndnois;     //!< Number of database noise records
        int                 nlrecs;     //!< Number of local records
        int                 nlraws;     //!< Number of local raw records
        int                 nledts;     //!< Number of local edit records
        int                 nlmods;     //!< Number of local model records
        int                 nlnois;     //!< Number of local noise records
        int                 nmerged;    //!< Experiments merged this scan
        bool                verify_pending; //!< A comparison pass is queued
        int                 dbg_level;  //!< Debug level
        QStringList         triples;    //!< List of triples
        QStringList         runIDs;     //!< List of run IDs
        QList< QStringList > runid_data; //!< List of run ID data

    private:
        //! \brief Start a phase of the scan
        //! \param text  What the phase is doing
        //! \param steps How many steps the phase has
        void scan_phase( const QString& text, int steps );

        //! \brief Put a message on the status line, cut to fit
        //! \param text The message
        void status_text( const QString& text );

        //! \brief Fit the tree columns without widening the window
        void fit_tree_columns( void );

        //! \brief Turn the controls a scan could be disturbed by on or off
        //! \param enabled True to turn them on
        void set_controls_enabled( bool enabled );

        //! \brief Tell the user what went wrong in the last scan
        void report_scan_errors( void );

        //! \brief Tell the user about records that share a GUID
        void review_duplicates( void );

    private slots:
        //! \brief Toggle the display of edits
        void toggle_edits( void );

        //! \brief Toggle the display of models
        void toggle_models( void );

        //! \brief Toggle the display of noises
        void toggle_noises( void );

        //! \brief The experiments are listed: draw the top of the tree
        void runs_listed( void );

        //! \brief One experiment's records are in: hang them under it
        //! \param index The position of the experiment
        void run_merged( int index );

        //! \brief Run the comparisons asked for outside a scan, in turn
        //! \param delay Milliseconds to wait first
        void schedule_verifies( int delay = 0 );

        //! \brief Compare the next experiment asked for outside a scan
        void pump_verifies( void );

        //! \brief Scan data
        void scan_data( void );

        //! \brief Reset the interface
        void reset( void );

        //! \brief Select an investigator
        void sel_investigator( void );

        //! \brief Assign an investigator
        //! \param index Index of the investigator
        void assign_investigator( int index );

        //! \brief Handle item click in the tree
        //! \param item Pointer to the tree widget item
        void clickedItem( QTreeWidgetItem* item );

        //! \brief Reset hide/show buttons
        //! \param edit Show edits button state
        //! \param model Show models button state
        //! \param noise Show noises button state
        //! \param browse Browse button state
        void reset_hsbuttons( bool edit, bool model, bool noise, bool browse );

        //! \brief Report the data status
        void reportDataStatus( void );

        //! \brief Handle selected run ID change
        //! \param runID Selected run ID
        void selected_runID( QString runID );

        //! \brief Handle selected run ID change
        //! \param index Index of the selected run ID
        void selected_runID( int index );

        //! \brief Show help information
        void help( void )
        { showHelp.show_help( "manage_data.html" ); };
};

#endif // US_MANAGE_DATA_H
