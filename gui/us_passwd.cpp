//! \file us_passwd.cpp
#include "us_passwd.h"
#include "us_global.h"
#include "us_settings.h"
#include "us_master_password.h"
#include "us_os_auth.h"
#include <QApplication>

// Window to anchor (and center) the password dialogs against: the owner's
// top-level window if it has a parent, otherwise the currently active window.
// US_Passwd objects are normally created parentless on the stack and never
// shown, so using 'this' as the parent makes Qt center the dialog over an
// unshown 0,0-positioned widget, i.e. in the top-left corner of the screen.
static QWidget* anchor_window( QWidget* self )
{
   QWidget* p = self->parentWidget();
   if ( p )
   {
      return p->window();
   }
   return QApplication::activeWindow(); // may be nullptr -> centered on screen
}

QString US_Passwd::getPasswd( void  )
{
  // Cached in shared memory, or read from the OS keychain if enabled.
  // All checks live in US_MasterPassword (utils); this class only prompts.
  QString err;
  QString pw = US_MasterPassword::obtainNonInteractive( &err );
  if ( ! pw.isEmpty() ) return pw;

  if ( ! err.isEmpty() && US_Settings::masterPwInKeychain() )
     qWarning() << "OS keychain:" << err;

  // A headless run has no one to answer the password dialog: use the
  // environment variable or a no-echo terminal prompt instead.
  if ( qApp != nullptr && qApp->property( "us3_headless" ).toBool() )
  {
     if ( ! US_MasterPassword::isSet() )
     {
        qWarning() << "Error: the master password has not been set.";
        return QString();
     }

     QString err_env;
     pw = US_MasterPassword::fromEnvironment( &err_env );
     if ( ! pw.isEmpty() ) return pw;

     QString err_tty;
     pw = US_MasterPassword::fromTerminal( &err_tty );
     if ( ! pw.isEmpty() ) return pw;

     qWarning().noquote()
        << "Error: the master password is not available to this headless run ("
        << err_env << "; " << err_tty << "). Run from a terminal, set"
        << US_MasterPassword::envVarName()
        << ", enable the OS keychain, start UltraScan to unlock the database,"
           " or use --no-db";
     return QString();
  }

  if ( ! US_MasterPassword::isSet() )
  {
     QMessageBox::information( anchor_window( this ), tr( "Password Error" ), tr( "The Master Password has not been set." ) );

     return QString();
  }

  // Ask the user to input the password
  while ( true ) 
  {
    bool ok;
    pw = QInputDialog::getText( anchor_window( this ),
                                tr( "Master Password" ),
                                tr( "Please input your Master Password" ),
                                QLineEdit::Password,
                                QString(),
                                &ok );

    // If the user cancelled, return a null string
    if ( ! ok ) return QString();

    // Verify, cache in shared memory and refresh the keychain copy
    if ( US_MasterPassword::accept( pw ) ) break;

    QMessageBox::information( anchor_window( this ), tr( "Password Error" ), tr( "The password is incorrect." ) );
  }

  return pw;
}


QMap < QString, QString >  US_Passwd::getPasswd_auditTrail( QString title, QString i_text, QString user_s )
{
  // // If the pw is in global memory, return it
  // QString pw = g.passwd();
  // if ( ! pw.isEmpty() ) return pw;

  QMap < QString, QString > form_map;
  QString pw;

  // Challenge: OS account password (opt-in) or UltraScan master password.
  // If OS authentication was requested but is not available in this build
  // or on this system, fall back to the master password.
  bool useOsAuth = US_Settings::auditUsesOsAuth();
  if ( useOsAuth && ! US_OsAuth::isAvailable() )
  {
     qWarning() << "OS authentication is not available; using the master password.";
     useOsAuth = false;
  }
  const QString pwField = useOsAuth ? QString( "Account Password:" )
                                    : QString( "Master Password:" );
  const QString osUser  = useOsAuth ? US_OsAuth::currentUser() : QString();

  // See if the master pasword has been set
  if ( ! useOsAuth && ! US_MasterPassword::isSet() )
  {
     QMessageBox::information( anchor_window( this ), tr( "Password Error" ), tr( "The Master Password has not been set." ) );

     return form_map;
  }

  bool reject_chosen = false;
  
  // Ask the user to input the password
  while ( true ) 
  {
     QDialog dialog( anchor_window( this ) );
     dialog.setWindowTitle( title );
     QFont        font_d = dialog.property( "font" ).value<QFont>();
     QFontMetrics fm( font_d );
     int          pixelsWide = fm.horizontalAdvance( dialog.windowTitle() );
     qDebug() << "pixelsWide -- " << pixelsWide;
     dialog.setMinimumWidth( pixelsWide * 3 );
     QFormLayout form( &dialog );

     // Add some text above the fields
     form.addRow( new QLabel( i_text ) );

     if ( useOsAuth )
     {
        QLabel* lb_os  = us_label( "OS Account:" );
        QLabel* lb_osv = us_label( osUser + "  (" + US_OsAuth::backendName() + ")" );
        lb_osv->setAccessibleName( "OS Account" );
        form.addRow( lb_os, lb_osv );
     }

     // Add the lineEdits with their respective labels
     QList<QLineEdit*> fields;
     QStringList       form_labels = { "User:", "Comment:", pwField };
     for ( int i = 0; i < form_labels.size(); ++i )
     {
        //QLineEdit *lineEdit = new QLineEdit(&dialog);
        QLineEdit* lineEdit = us_lineedit();
        //QString label = form_labels[i];

        QLabel*    label = us_label( form_labels[i] );

        lineEdit->setObjectName( form_labels[i] );
        lineEdit->setAccessibleName( form_labels[i].left( form_labels[i].length() - 1 ) );
        label->setBuddy( lineEdit );

        if ( form_labels[i] == pwField )
        {
           lineEdit->setEchoMode( QLineEdit::Password );
        }


        if ( form_labels[i] == "User:" )
        {
           lineEdit->setText( user_s );
           us_setReadOnly( lineEdit, true );
        }

        form.addRow( label, lineEdit );

        fields << lineEdit;
     }
    //for e-Sigs, add RadioButtons Approve | Reject
    QList<QRadioButton*> fields_radio;
    QStringList form_labels_radio = { "Approve", "Reject" }; 
    if( title.contains("GMP e-Signing Form") )
      {
	QHBoxLayout *hLayout = new QHBoxLayout();
	for(int i = 0; i < form_labels_radio.size(); ++i)
	  {
	    QRadioButton *radioB = new QRadioButton(form_labels_radio[i]);
	    radioB->setObjectName( form_labels_radio[i] );

	    if ( form_labels_radio[i] == QString("Approve") )
	      radioB->setChecked(true);
	    
	    hLayout->addWidget(radioB);
	    fields_radio << radioB;
	  }
	form.addRow( "DECISION:", hLayout);
      }  
    // Add some standard buttons (Cancel/Ok) at the bottom of the dialog
    QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
			       Qt::Horizontal, &dialog);
    form.addRow(&buttonBox);
    connect(&buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    
    // Show the dialog as modal
    if (dialog.exec() == QDialog::Accepted)
      {
	// If the user didn't dismiss the dialog, do something with the fields
	foreach(QLineEdit * lineEdit, fields)
	  {
	    if ( lineEdit->objectName() == pwField )
	      {
		pw = lineEdit->text();
		// Never hand the OS account password to the caller
		if ( useOsAuth )
		  continue;
	      }
	    else
	      qDebug() << lineEdit->text();

	    form_map[ lineEdit->objectName() ] = lineEdit->text();
	  }
	//for e-Sigs, capture Approve/Reject status to form_map
	foreach(QRadioButton * radioB, fields_radio)
	  {
	    if ( radioB->isChecked() )
	      {
		qDebug() << "RadioB checked-- " << radioB->objectName();
		form_map[ "Decision:" ] = radioB->objectName();
	      }
	  }
      }
    else 
      {
	form_map. clear();
	reject_chosen = true;
      }

    //break if Cancelled
    if ( reject_chosen )
      break;
    
    if ( useOsAuth )
    {
       // Check the comment first: every failed OS check may count towards
       // an account lockout (e.g. pam_faillock).
       if ( form_map[ "Comment:" ].isEmpty() )
       {
          QMessageBox::critical( anchor_window( this ),
                                 tr( "No Comment" ),
                                 tr( "<font color='red'><b>ERROR:</b></font> The Comment field is empty." ) );
          continue;
       }

       QString err;
       const bool ok = US_OsAuth::verifyCurrentUser( pw, &err );
       pw.fill( QChar( 0 ) );
       pw.clear();
       if ( ok )
         break;

       QMessageBox::critical( anchor_window( this ),
                              tr( "Password Error" ),
                              tr( "<font color='red'><b>ERROR:</b></font> The password for "
                                  "OS account %1 could not be verified:<br>%2" )
                              .arg( osUser.toHtmlEscaped(), err.toHtmlEscaped() ) );
       continue;
    }

    // Check against the stored master-password hash
    if ( US_MasterPassword::verify( pw ) && !form_map["Comment:"].isEmpty()  )
      break;

    QMessageBox::critical( anchor_window( this ),
                           tr( "No Comment and/or Password Error" ),
                           tr( "<font color='red'><b>ERROR:</b></font> The Comment field is empty, "
                               "or the password is incorrect." ) );
  }

  // // Save the password in global memory and return
  // g.setPasswd( pw );
  
  return form_map;
}
