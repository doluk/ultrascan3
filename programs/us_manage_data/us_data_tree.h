//! \file us_data_tree.h
#ifndef US_DATA_TREE_H
#define US_DATA_TREE_H

#include "us_extern.h"
#include "us_sync_exper.h"
#include "us_data_model.h"
#include "us_data_process.h"
#include "us_db2.h"
#include "us_model.h"
#include "us_buffer.h"
#include "us_analyte.h"
#include "us_help.h"
#include "us_dataIO.h"

//! \class US_DataTree
//! \brief Class to manage the data tree, including building, expanding, and handling context menus.
class US_DataTree : public QObject
{
    Q_OBJECT

    public:
        //! \brief Constructor for US_DataTree
        //! \param model Pointer to US_DataModel object
        //! \param tree Pointer to QTreeWidget object
        //! \param parent Optional parent widget
        US_DataTree( US_DataModel* model, QTreeWidget* tree, QWidget* parent = 0 );

        //! \brief Toggle the expansion of tree items
        //! \param text Text of the tree item
        //! \param expand True to expand, false to collapse
        void toggle_expand( QString text, bool expand );

        //! \brief Build the first layer of the data tree
        //!
        //! Only the experiment rows are built.  What hangs off an
        //! experiment is added by \ref fill_run when the scan reaches it,
        //! or when the user opens it.
        void build_dtree( void );

        //! \brief Add the records of one experiment under its row
        //! \param runIndex The position of the experiment
        void fill_run( int runIndex );

        //! \brief Colour one experiment's rows again
        //!
        //! A scan compares the records of an experiment after its rows are
        //! already in the tree, so this is how a row that turns out to be a
        //! conflict stops reading as in sync.
        //! \param runIndex The position of the experiment
        void refresh_run( int runIndex );

    public slots:
                //! \brief Show help information for the data tree
                void dtree_help( void );

        //! \brief Show context menu for a tree item
        //! \param item Pointer to the tree widget item
        void row_context_menu( QTreeWidgetItem* item );

        //! \brief Read an experiment the user opened
        //!
        //! An experiment the layered scan has not reached yet is read now,
        //! ahead of the ones still queued, so opening a row never shows an
        //! empty branch.
        //! \param item The tree item that was opened or selected
        void item_opened( QTreeWidgetItem* item );

    private:
        US_DataModel*    da_model;     //!< Data model object
        QTreeWidget*     tw_recs;      //!< Tree widget
        QPushButton*     pb_hsedit;    //!< Pointer to edits hide/show button
        QPushButton*     pb_hsmodl;    //!< Pointer to models hide/show button
        QPushButton*     pb_hsnois;    //!< Pointer to noises hide/show button
        QWidget*         parentw;      //!< Pointer to parent widget
        US_DataProcess*  da_process;   //!< Data processor object
        QTreeWidgetItem* tw_item;      //!< Current tree widget item
        QList< QTreeWidgetItem* > runitems; //!< One item per experiment
        US_DataModel::DataDesc cdesc;  //!< Current record description
        QList< QTreeWidgetItem* > selitems; //!< All selected items
        QVector< int > selrows;        //!< All selected rows
        QVector< int > actrows;        //!< All action rows
        QVector< int > rawrows;        //!< Raw selected rows
        QLabel*       lb_status;       //!< Status label
        QTextEdit*    te_status;       //!< Status text edit
        QLineEdit*    le_invtor;       //!< Investigator line edit
        QPushButton*  pb_browse;       //!< Browse button
        QPushButton*  pb_detail;       //!< Detail button
        QPushButton*  pb_reset;        //!< Reset button
        QPushButton*  pb_help;         //!< Help button
        QPushButton*  pb_close;        //!< Close button

        int ntrows;                    //!< Number of tree rows
        int ntcols;                    //!< Number of tree columns
        int ncrecs;                    //!< Number of current records
        int ncraws;                    //!< Number of current raw records
        int ncedts;                    //!< Number of current edit records
        int ncmods;                    //!< Number of current model records
        int ncnois;                    //!< Number of current noise records
        int ndrecs;                    //!< Number of database records
        int ndraws;                    //!< Number of database raw records
        int ndedts;                    //!< Number of database edit records
        int ndmods;                    //!< Number of database model records
        int ndnois;                    //!< Number of database noise records
        int nlrecs;                    //!< Number of local records
        int nlraws;                    //!< Number of local raw records
        int nledts;                    //!< Number of local edit records
        int nlmods;                    //!< Number of local model records
        int nlnois;                    //!< Number of local noise records
        int kdmy;                      //!< Dummy variable
        int dbg_level;                 //!< Debug level

    private:
        //! \brief The color, type name and source text of a record
        //! \param desc The record to look at
        //! \param fbru Filled in with the foreground brush
        //! \param rtyp Filled in with the record type name
        //! \param rsrc Filled in with the source text
        void row_look( const US_DataModel::DataDesc& desc, QBrush& fbru,
                       QString& rtyp, QString& rsrc );

        //! \brief Add one record to the per-type counters
        //! \param desc The record to count
        void count_row( const US_DataModel::DataDesc& desc );

        //! \brief The look of a record, and the record counted
        //! \param desc The record to classify
        //! \param fbru Filled in with the foreground brush
        //! \param rtyp Filled in with the record type name
        //! \param rsrc Filled in with the source text
        void classify_row( const US_DataModel::DataDesc& desc, QBrush& fbru,
                           QString& rtyp, QString& rsrc );

    private slots:
                //! \brief Upload a tree item
                void item_upload( void );

        //! \brief Download a tree item
        void item_download( void );

        //! \brief Remove a tree item from the database
        void item_remove_db( void );

        //! \brief Remove a tree item from local storage
        void item_remove_loc( void );

        //! \brief Remove all selected tree items
        void item_remove_all( void );

        //! \brief Show details of a tree item
        void item_details( void );

        //! \brief Show details of all selected tree items
        void items_details( void );

        //! \brief Get the action rows
        //! \return Number of action rows
        int action_rows( void );

        //! \brief Get the record type
        //! \param recType Record type
        //! \param recTypeStr Record type string
        void record_type( int recType, QString& recTypeStr );

        //! \brief Perform actions on selected tree items
        //! \param action Action to perform
        //! \param text Action text
        //! \return Status code of the operation
        int do_actions( QString action, QString text );

        bool check_filename_for_autoflow( QString );
        bool is_modelIDs_from_autoflow  ( QString );
        bool parse_models_desc_json( QStringList, QString );

        //! \brief Get the action text
        //! \param action Action to perform
        //! \param text Action text
        //! \return Action text
        QString action_text( QString action, QString text );

        //! \brief Handle the result of an action
        //! \param result Result code
        //! \param text Result text
        void action_result( int result, QString text );

        //! \brief Get the record state
        //! \param state Record state
        //! \return Record state string
        QString record_state( int state );

        //! \brief Check for significant differences between data descriptions
        //! \param desc Data description
        //! \param db Pointer to US_DB2 object
        //! \return True if there are significant differences, false otherwise
        bool significant_diffs( US_DataModel::DataDesc desc, US_DB2* db );
};

#endif // US_DATA_TREE_H
