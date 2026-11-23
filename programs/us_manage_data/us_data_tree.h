//! \file us_data_tree.h
#ifndef US_DATA_TREE_H
#define US_DATA_TREE_H

#include "us_extern.h"
#include "us_data_model.h"
#include "us_data_process.h"
#include "us_db2.h"
#include "us_model.h"
#include "us_help.h"
#include "us_dataIO.h"

#include <functional>

//! \class US_DataTree
//! \brief Class to manage the data tree, including building, expanding, and handling context menus.
class US_DataTree : public QObject
{
    Q_OBJECT

    public:
        //! \brief Statuses of an action that are not an error
        enum ActStatus
        {
            ACT_IN_SYNC   = 996,  //!< A DB replace found nothing to replace
            ACT_REFUSED   = 997,  //!< Refused, and the user was told why
            ACT_NOTHING   = 998,  //!< No selected record could take it
            ACT_CANCELLED = 999   //!< The user cancelled
        };

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

        //! \brief Add the row of an experiment added after the tree was built
        //!
        //! The stand-in experiment of the orphan records is one.
        //! \param runIndex The position of the experiment
        void add_run( int runIndex );

        //! \brief Add the records of one experiment under its row
        //! \param runIndex The position of the experiment
        void fill_run( int runIndex );

        //! \brief Show an experiment's records again, from its new rows
        //!
        //! What US_DataModel::runReloaded calls for: the items of the rows
        //! it left behind go, and the new rows are shown.
        //! \param runIndex The position of the experiment
        void refill_run( int runIndex );

        //! \brief Colour one experiment's rows again, and count again
        //!        what is under each of them
        //!
        //! A scan compares the records of an experiment after its rows are
        //! already in the tree, so this is how a row that turns out to be a
        //! conflict stops reading as unverified; and an action changes
        //! where records are, which the counts columns show.
        //! \param runIndex The position of the experiment
        void refresh_run( int runIndex );

        /*! \brief Act on the selected rows

            What a context menu action does once confirmed: the selection
            is the tree's, and the action rows are the selected records
            and their descendants (see \ref action_row_list).
            \param item_exs Where the records exist ("Database only", ...)
            \param item_act The action ("DB-only remove", ...)
            \returns 0 on success, an error status, \ref ACT_NOTHING when
                     no record could be acted on, or \ref ACT_REFUSED when
                     the GMP guard refused
        */
        int act_on_selection( QString item_exs, QString item_act );

        //! \brief What a prompt is for
        enum PromptKind
        {
            PROMPT_INFORM  = 0,   //!< A message; the answer is ignored
            PROMPT_CONFIRM = 1    //!< A yes/no question; true for yes
        };

        /*! \brief Replace the message boxes an action shows

            By default an action tells the user why it refused, and asks
            before it removes everything under a stand-in experiment, with
            message boxes.  A caller that runs actions without a user -- a
            test -- answers them here instead.
            \param func Called with the kind, the title and the text; its
                        result is the answer to a PROMPT_CONFIRM
        */
        void setPrompter(
            std::function< bool( int, const QString&, const QString& ) > func );

        //! \brief The rows the selection acts on
        //!
        //! The selected records and every descendant of them, in tree
        //! order, with raw records left out (they are never acted on).
        QVector< int > action_row_list( void );

    public slots:
        //! \brief Show help information for the data tree
        void dtree_help( void );

        //! \brief Show context menu for a tree item
        //! \param item Pointer to the tree widget item
        void row_context_menu( QTreeWidgetItem* item );

        /*! \brief Read an experiment the user opened

            An experiment the layered scan has not reached yet is read now,
            ahead of the ones still queued, and is asked to be compared;
            the comparison itself is left to whoever runs the queue, so
            opening rows never blocks.  While a scan runs, both requests
            are only queued.
            \param item The tree item that was opened
        */
        void item_opened( QTreeWidgetItem* item );

    private:
        US_DataModel*    da_model;     //!< Data model object
        QTreeWidget*     tw_recs;      //!< Tree widget
        QWidget*         parentw;      //!< Pointer to parent widget
        US_DataProcess*  da_process;   //!< Data processor object
        QTreeWidgetItem* tw_item;      //!< Current tree widget item
        QList< QTreeWidgetItem* > runitems; //!< One item per experiment
        US_DataModel::DataDesc cdesc;  //!< Current record description
        QList< QTreeWidgetItem* > selitems; //!< All selected items
        QVector< int > selrows;        //!< All selected rows
        QVector< int > actrows;        //!< All action rows
        QVector< int > rawrows;        //!< Raw selected rows
        QMap< QString, bool > gmp_runs;     //!< Runs the GMP guard checked
        QStringList    gmp_models;     //!< Model IDs produced by autoflow
        bool           gmp_models_read; //!< True once gmp_models is read

        //! Whether a model or noise conflict is more than formatting, by
        //! GUID and compared contents: significant_diffs() reads both copies
        QHash< QString, bool > sigdiffs;

        //! Answers prompts instead of message boxes, when set
        std::function< bool( int, const QString&, const QString& ) > prompter;

        int ntcols;                    //!< Number of tree columns
        int dbg_level;                 //!< Debug level

    private:
        //! \brief The color, type name and source text of a record
        //! \param desc The record to look at
        //! \param fbru Filled in with the foreground brush
        //! \param rtyp Filled in with the record type name
        //! \param rsrc Filled in with the source text
        void row_look( const US_DataModel::DataDesc& desc, QBrush& fbru,
                       QString& rtyp, QString& rsrc );

        //! \brief The color, type name and source text of an experiment
        //!
        //! An experiment in both places reads as what its records read as;
        //! a stand-in experiment reads as neither place.
        void run_look( int runIndex, QBrush& fbru, QString& rtyp,
                       QString& rsrc );

        //! \brief Colour an experiment row from what is under it now, and
        //!        show its counts
        void refresh_run_item( int runIndex );

        //! \brief The "Children" and "Descendants" texts of a record row
        //! \param row     The row of the record
        //! \param lastRow The last row of its experiment
        //! \param anch    Filled in with the children counts
        //! \param ande    Filled in with the descendants counts
        void row_counts( int row, int lastRow, QString& anch, QString& ande );

        //! \brief Show a message, or ask a question; see \ref setPrompter
        bool ask_user( int kind, const QString& title, const QString& text );

        //! \brief Make sure an experiment's records are read

        //! \param runIndex The position of the experiment
        //! \returns True when the experiment has its records
        bool ensure_loaded( int runIndex );

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

    private:
        //! \brief A batch of record changes (US_DataModel::begin_changes),
        //!        ended by \ref end or when it goes out of scope
        class ChangeBatch
        {
            public:
                explicit ChangeBatch( US_DataModel* model );
                ~ChangeBatch();

                //! \brief End the batch now; later calls do nothing
                void end( void );

            private:
                US_DataModel* da_model;
        };

        //! \brief Ask for, then perform, one context menu action
        //! \param item_exs Where the records exist
        //! \param item_act The action
        void confirm_and_act( QString item_exs, QString item_act );

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

        //! \brief True when a run is required by the GMP framework
        //! \param runID The run ID
        bool check_filename_for_autoflow( const QString& runID );

        //! \brief True when a model was produced by an autoflow
        //! \param mID The model ID
        bool is_modelIDs_from_autoflow( const QString& mID );

        //! \brief The model IDs in a list of autoflow model descriptions
        //! \param modelInfos The JSON model descriptions
        static QStringList parse_models_desc_json( const QStringList& modelInfos );

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
        //! \param db Pointer to the database connection
        //! \return True if there are significant differences, false otherwise
        bool significant_diffs( US_DataModel::DataDesc desc, IUS_DB2* db );
};

#endif // US_DATA_TREE_H
