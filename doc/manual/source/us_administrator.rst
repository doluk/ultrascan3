====================================
UltraScan-III Administrator 
====================================

.. toctree:: 
  :maxdepth: 3 

.. contents:: Index
    :local: 

Master Password
===============

The master password encrypts the database passwords stored in your
UltraScan settings. Set or change it with **Change Master Password**.

Security Options
----------------

Both options are off by default. To change only the options, enter the
current password and leave the new password fields empty.

**Unlock with OS login**
   The master password is kept in the operating system's secret store
   (macOS Keychain, Windows Credential Manager, or the Secret Service /
   KWallet on Linux). It is unlocked by your OS login and survives changes
   of your OS password. If the secret store is not available, for example
   after an administrator reset your OS password, UltraScan asks for the
   master password as before and stores it again.

**Confirm audit-trail / e-signature forms with the OS account password**
   GMP audit-trail and e-signature forms ask for the password of the
   logged-in OS account instead of the master password.

Command-Line Tools
------------------

Without a GUI (``--errors-cl``), database access takes the master
password from a running, unlocked UltraScan, the OS keychain (if
enabled), the environment variable ``US3_MASTER_PASSWORD``, or a
no-echo terminal prompt, in this order.
