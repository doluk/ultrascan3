//! \file us_admin.h
#ifndef US_ADMIN_H
#define US_ADMIN_H

#include "us_widgets.h"

//! A class to allow the user to set or change the user's Master Password
class US_Admin : public US_Widgets
{
  Q_OBJECT
  
  public:
    //! Construct the window to manage the user's Master Password
    //! When setting the password, it is saved in global memory. See \ref US_Global.
    //! A cryptographic hash of the password is saved in the user's
    //! settings. See \ref US_Settings.
    //! \param w Pointer to the parent widget, defaults to nullptr
    //! \param flags Qt::WindowFlags, defaults to Qt::WindowFlags()
    US_Admin( QWidget* w = nullptr, Qt::WindowFlags flags = Qt::WindowFlags() );
    //! A null destructor. 
    ~US_Admin() {};
    
  private:
    QLineEdit*   le_oldPasswd;
    QLineEdit*   le_passwd1;
    QLineEdit*   le_passwd2;

    QCheckBox*   ck_keychain;  //!< keep master password in OS keychain
    QCheckBox*   ck_osAudit;   //!< OS account password for audit trail

    QPushButton* pb_help;
    QPushButton* pb_save;
    QPushButton* pb_cancel;
    
    QString      password1;    
    QString      password2;    
    
    //! Apply the keychain / audit-trail options for master password \p pw.
    //! \retval false if the keychain could not be updated
    bool applySecurityOptions( const QString& pw );

  private slots:
    void help();
    void save();
};

#endif


