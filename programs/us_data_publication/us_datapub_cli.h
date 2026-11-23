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
      //! The window is what starts without arguments.  Any long option
      //! (\c --mode, \c --no-ui, \c --help, \c --version, but also a
      //! misspelt one) selects the command line instead, so that arguments
      //! meant for it are reported when they are wrong rather than
      //! silently opening the window.  Single-dash options are left to Qt.
      //! \param arguments The arguments the program was started with
      static bool wanted( const QStringList& arguments );

      //! \brief Run the command line
      //! \param arguments The arguments the program was started with
      //! \returns The process exit code: 0 on success
      int run( const QStringList& arguments );

      //! \brief The usage text
      static QString usage( void );

      /*! \brief A password as given, without the line end that ends it

          One trailing "\n", "\r\n" or "\r" is removed and nothing else:
          spaces are part of a password.  The same rule holds for a password
          file, the US3_DB_PASSWORD environment variable and the prompt.
          \param text The line read
      */
      static QString chopLineEnd( const QString& text );

      /*! \brief Read the master password from the first line of a file
          \param path     The password file
          \param password Filled in with the password
          \param warning  Filled in when the file can be read by other users
                          (Unix: group or world readable)
          \param error    Filled in when the file cannot be read
          \returns True when the file could be read
      */
      static bool readPasswordFile( const QString& path, QString& password,
                                    QString& warning, QString& error );

      /*! \brief The run an \c --experiment-id value names

          A run ID that matches exactly always wins: a run ID made only of
          digits ("12345") is not taken for the database ID of some other
          run.  Only when no run has that run ID is the value taken as a
          database ID, and then only when exactly one run has it -- local
          copies of runs can carry the same placeholder ID.
          \param runs   The runs of the source
          \param wanted The value given
          \param why    On failure, why; on success, a warning when the
                        value is also the database ID of another run
          \returns The run ID of the run, or an empty string
      */
      static QString resolveRun( const QList< US_DataPubCatalog::Run >& runs,
                                 const QString& wanted, QString& why );

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
            QString                  includeScope;
            QString                  conflict;
            QMap< QString, QString > typeConflicts;
            QString                  renameSuffix;
            QString                  comment;
            QString                  dbPassword;
            QString                  dbPasswordFile;
            bool                     timeState;
            bool                     project;
            bool                     dryRun;
            bool                     verbose;
            bool                     nonInteractive;
            bool                     verify;
            bool                     listOnly;
            bool                     strictHardware;
            bool                     help;
            bool                     version;

            //! Every long option given, as it was spelt
            QStringList              given;
      };

      Args        args;
      bool        verbose;
      //! Set by password() when a password file or the terminal could not
      //! be read -- an I/O failure, not a usage error
      bool        password_failed;
      QTextStream out;
      QTextStream err;

      bool parse( const QStringList& arguments, QString& error );
      int  runExport( void );
      int  runImport( void );

      /*! \brief The master password, when \a needed

          From --db-password-file, --db-password (with a warning: it is
          visible to other users in the process list), the US3_DB_PASSWORD
          environment variable or, last, an interactive prompt on the error
          output that does not echo what is typed (and puts the terminal
          back when interrupted).  A non-interactive run that has none of
          them, or a Windows run whose input is not a console, gets an
          empty string and a message saying how to give one.
      */
      QString password( bool needed );
      void    print( const QString& );
      void    fail ( const QString& );
};
#endif
