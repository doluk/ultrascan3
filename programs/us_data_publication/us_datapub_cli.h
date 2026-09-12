//! \file us_datapub_cli.h
#ifndef US_DATAPUB_CLI_H
#define US_DATAPUB_CLI_H

#include <QtCore>

#include "us_datapub_defs.h"
#include "us_datapub_export.h"
#include "us_datapub_import.h"

/*! \class US_DataPubCLI
    \brief The command line front end of the data publication tool

    The command line covers the same ground as the two tabs of the window:
    it exports a bundle from the database or from a local disk store, and it
    imports one into either.  It never asks anything it was not told to ask,
    which makes it usable from a script or a scheduled job.
*/
class US_DataPubCLI : public QObject
{
   Q_OBJECT

   public:
      //! \brief Construct the command line front end
      //! \param parent The parent object, for Qt ownership
      explicit US_DataPubCLI( QObject* parent = nullptr );

      //! \brief True when the arguments ask for the command line
      //!
      //! The window is what starts without arguments; \c --mode, \c --no-ui,
      //! \c --help and \c --version all select the command line instead.
      //! \param arguments The arguments the program was started with
      static bool wanted( const QStringList& arguments );

      //! \brief Run the command line
      //! \param arguments The arguments the program was started with
      //! \returns The process exit code: 0 on success
      int run( const QStringList& arguments );

      //! \brief The usage text
      static QString usage( void );

   private slots:
      void onMessage( const QString& );

   private:
      class Args
      {
         public:
            Args();

            QString                  mode;
            QString                  bundle;
            QString                  source;
            QString                  target;
            QString                  outputDir;
            QString                  projectID;
            QString                  projectGUID;
            QStringList              experiments;
            QStringList              modelGUIDs;
            QStringList              noiseGUIDs;
            QString                  scope;
            QString                  conflict;
            QMap< QString, QString > typeConflicts;
            QString                  renameSuffix;
            QString                  comment;
            QString                  dbPassword;
            bool                     timeState;
            bool                     dryRun;
            bool                     verbose;
            bool                     nonInteractive;
            bool                     verify;
            bool                     listOnly;
            bool                     help;
            bool                     version;
      };

      Args        args;
      bool        verbose;
      QTextStream out;
      QTextStream err;

      bool parse( const QStringList& arguments, QString& error );
      int  runExport( void );
      int  runImport( void );

      QString password( bool needed );
      void    print( const QString& );
      void    fail ( const QString& );
};
#endif
