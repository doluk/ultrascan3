//! \file us_admin.cpp
#include "us_admin.h"
#include "us_license_t.h"
#include "us_help.h"
#include "us_settings.h"
#include "us_gui_settings.h"
#include "us_crypto.h"
#include "us_os_keychain.h"
#include "us_master_password.h"
#include "us_os_auth.h"

US_Admin::US_Admin( QWidget* w, Qt::WindowFlags flags ) 
   : US_Widgets( true, w, flags )
{
  setWindowTitle( "Change Master Password" );
  setAttribute( Qt::WA_DeleteOnClose );

  int buttonh = 26;
  
  setPalette( US_GuiSettings::frameColor() );
  
  QLabel* header = us_banner( "Change Master Password" );
  header->setMinimumHeight( buttonh * 3 / 2 );
    
  QString oldPass = US_Settings::UltraScanPW();
  QLabel* oldPW = NULL;

  if ( ! oldPass.isEmpty() ) 
  {
    oldPW = us_label( "Old Password:" );
    oldPW->setMinimumHeight( buttonh );

    le_oldPasswd = us_lineedit( "" );
    le_oldPasswd->setEchoMode( QLineEdit::Password );
    le_oldPasswd->setMinimumHeight( buttonh );
  }
  else
    le_oldPasswd = NULL;

  QLabel* passwd1 = us_label( "Enter New Password:" );
  passwd1->setMinimumHeight( buttonh );
    
  le_passwd1 = us_lineedit( "" );
  le_passwd1->setEchoMode( QLineEdit::Password );
  le_passwd1->setMinimumHeight( buttonh );
  le_passwd1->setMinimumWidth( 300 );
  
  QLabel* passwd2 = us_label( "Verify New Password:" );
  passwd2->setMinimumHeight(buttonh);
  
  le_passwd2 = us_lineedit( "" );
  le_passwd2->setMinimumHeight( buttonh );
  le_passwd2->setEchoMode( QLineEdit::Password );
  
  // Optional OS integration. The master password remains the fallback.
  QGridLayout* lo_keychain = us_checkbox(
     tr( "Unlock with OS login: keep the master password in the %1" )
     .arg( US_OsKeychain::backendName() ), ck_keychain,
     US_Settings::masterPwInKeychain() && US_OsKeychain::isAvailable() );
  ck_keychain->setEnabled( US_OsKeychain::isAvailable() );
  ck_keychain->setToolTip( US_OsKeychain::isAvailable()
     ? tr( "The master password is read from the OS keychain, which is unlocked by "
           "your OS login and survives OS password changes. If the keychain is "
           "unavailable you are asked for the master password." )
     : tr( "This build of UltraScan has no OS keychain support." ) );

  QGridLayout* lo_osAudit = us_checkbox(
     tr( "Confirm audit-trail / e-signature forms with the OS account password" ),
     ck_osAudit, US_Settings::auditUsesOsAuth() && US_OsAuth::isAvailable() );
  ck_osAudit->setEnabled( US_OsAuth::isAvailable() );
  ck_osAudit->setToolTip( US_OsAuth::isAvailable()
     ? tr( "Backend: %1, account: %2" )
       .arg( US_OsAuth::backendName(), US_OsAuth::currentUser() )
     : tr( "No OS authentication backend is available on this system." ) );

  QLabel* lb_hint = us_label(
     tr( "To change only the options above, enter the current password "
         "and leave the new password fields empty." ) );
  lb_hint->setWordWrap( true );

  pb_help = us_pushbutton( "Help" );
  pb_help->setMinimumHeight( buttonh );
  connect( pb_help, &QAbstractButton::clicked, this, &US_Admin::help );
  
  pb_save = us_pushbutton( "Save" );
  pb_save->setMinimumHeight( buttonh ); 
  connect( pb_save, &QAbstractButton::clicked, this, &US_Admin::save );
          
  pb_cancel = us_pushbutton( "Close" );
  pb_cancel->setMinimumHeight( buttonh );
  connect( pb_cancel, &QAbstractButton::clicked, this, &QWidget::close );

  // Layout
  QGridLayout* passwords = new QGridLayout;
  passwords->addWidget( header, 0, 0, 1, 2 );

  int row = 1;
  if ( ! oldPass.isEmpty() )
  {
    passwords->addWidget( oldPW       , row,   0 );
    passwords->addWidget( le_oldPasswd, row++, 1 );
  }

  passwords->addWidget( passwd1   , row,   0 );
  passwords->addWidget( le_passwd1, row++, 1 );
  passwords->addWidget( passwd2   , row,   0 );
  passwords->addWidget( le_passwd2, row++, 1 );
  passwords->addLayout( lo_keychain, row++, 0, 1, 2 );
  passwords->addLayout( lo_osAudit , row++, 0, 1, 2 );
  if ( ! oldPass.isEmpty() )
    passwords->addWidget( lb_hint  , row++, 0, 1, 2 );
  
  QHBoxLayout* buttonLine = new QHBoxLayout;
  buttonLine->addWidget( pb_help   );
  buttonLine->addWidget( pb_save   );
  buttonLine->addWidget( pb_cancel );
  
  QVBoxLayout* main = new QVBoxLayout;
  main->setSpacing( 2 );
  main->addLayout( passwords );
  main->addLayout( buttonLine );

  setLayout( main );
}

void US_Admin::save( void )
{
  QByteArray oldPW = US_Settings::UltraScanPW();

  if ( ! oldPW.isEmpty() ) 
  {
    if ( ! US_MasterPassword::verify( le_oldPasswd->text() ) )
    {
      QMessageBox::information( this, 
        tr( "Attention:" ), 
        tr( "The old password is incorrect. Please re-input.\n" ) );
      return;
    }
  }

  // Options only: current password verified above, no new password given
  if ( ! oldPW.isEmpty()
       && le_passwd1->text().isEmpty() && le_passwd2->text().isEmpty() )
  {
    const QString curPW = le_oldPasswd->text();
    if ( applySecurityOptions( curPW ) )
    {
      US_MasterPassword::accept( curPW );
      close();
    }
    return;
  }

  if ( le_passwd1->text() != le_passwd2->text() )
  {
    le_passwd1->setText("");
    le_passwd2->setText("");

    QMessageBox::information( this, 
        tr( "Attention:" ), 
        tr( "The entered passwords are not same. Please re-input.\n" ) );
    return;
  }

  QString newPW = le_passwd1->text();

  if ( newPW.isEmpty() )
  {
    QMessageBox::information( this, 
        tr( "Attention:" ), 
        tr( "The new password cannot be empty. Please re-input.\n" ) );
    return;
  }


  // We need to reset any passwords in databases here
  // from le_oldPasswd->text()
  // to   le_passwd1->text()

  QStringList defaultDB = US_Settings::defaultDB();
  QString     oldPass;
  
  if ( le_oldPasswd != NULL ) oldPass = le_oldPasswd->text();

  // Helper lambdas: safely re-encrypt one record
  auto reencryptRecord = [&](QStringList& rec) -> bool
  {
    // Expect: 4=cipher, 5=iv/meta (db password), 7=cipher, 8=iv/meta (investigator password)
    if ( rec.size() < 6 ) {
      // nothing to do / malformed list handled elsewhere
      return true;
    }
    // reencrypt database password
    const QString cipherHex = rec.at( 4 );
    const QString ivHex     = rec.at( 5 );
    if ( cipherHex.isEmpty() ) {
        return true;
    }

    const QString plain = US_Crypto::decrypt( cipherHex, oldPass, ivHex );

    // If there was something encrypted, decryption must succeed; otherwise abort.
    if ( !cipherHex.isEmpty() && plain.isEmpty() ) {
      return false;
    }

    const QStringList newCt = US_Crypto::encrypt( plain, newPW );
    if ( newCt.size() < 2 ) {
      return false;
    }

    rec.replace(4, newCt.at(0));
    rec.replace(5, newCt.at(1));

    if ( rec.size() < 9 ) {
      return true;
    }

    // reencrypt user password if needed
    const QString cipherHex2 = rec.at( 7 );
    const QString ivHex2     = rec.at( 8 );

    const QString plain2 = US_Crypto::decrypt( cipherHex2, oldPass, ivHex2 );

    // If there was something encrypted, decryption must succeed; otherwise abort.
    if ( !cipherHex2.isEmpty() && plain2.isEmpty() ) {
      return false;
    }

    const QStringList newCt2 = US_Crypto::encrypt( plain2, newPW );
    if ( newCt2.size() < 2 ) {
      return false;
    }

    rec.replace(7, newCt2.at(0));
    rec.replace(8, newCt2.at(1));

    return true;
  };

  if ( !defaultDB.empty() )
  {
    if ( !reencryptRecord( defaultDB ) ) {
      QMessageBox::information(this, tr("Attention:"),
    tr("Password change failed: could not decrypt an existing entry.\n"
     "No entries were modified."));
      return;
    }
  }

  QList<QStringList> databases = US_Settings::databases();

  for ( auto& database : databases )
  {
    if ( !reencryptRecord( database ) ) {
      QMessageBox::information(this, tr("Attention:"),
    tr("Password change failed: could not decrypt an existing entry.\n"
     "No entries were modified."));
      return;
    }
  }

  /// commit updated settings

  US_Settings::set_UltraScanPW( US_MasterPassword::hash( newPW ) );

  if ( !defaultDB.empty() ) {
    US_Settings::set_defaultDB( defaultDB );
  }

  if ( !databases.empty() ) {
    US_Settings::set_databases( databases );
  }
  
  US_MasterPassword::accept( newPW );   // cache in shared memory
  applySecurityOptions( newPW );
  close();
}

bool US_Admin::applySecurityOptions( const QString& pw )
{
  const bool useKeychain = ck_keychain->isEnabled() && ck_keychain->isChecked();
  const bool useOsAudit  = ck_osAudit ->isEnabled() && ck_osAudit ->isChecked();
  bool ok = true;
  QString err;

  if ( useKeychain )
  {
    if ( US_OsKeychain::writeSecret( US_OsKeychain::masterPasswordKey(), pw, &err ) )
      US_Settings::set_masterPwInKeychain( true );
    else
    {
      ok = false;
      US_Settings::set_masterPwInKeychain( false );
      QMessageBox::warning( this, tr( "OS Keychain" ),
        tr( "The master password could not be stored in the %1:\n%2\n\n"
            "You will be asked for the master password as before." )
        .arg( US_OsKeychain::backendName(), err ) );
    }
  }
  else
  {
    if ( US_Settings::masterPwInKeychain() )
    {
      if ( ! US_OsKeychain::deleteSecret( US_OsKeychain::masterPasswordKey(), &err ) )
        qWarning() << "Could not remove the master password from the OS keychain:" << err;
    }
    US_Settings::set_masterPwInKeychain( false );
  }

  US_Settings::set_auditUsesOsAuth( useOsAudit );
  return ok;
}

void US_Admin::help()
{
  US_Help* online_help = new US_Help( this );
  online_help->show_help( "manual/us_administrator.html" );
}

