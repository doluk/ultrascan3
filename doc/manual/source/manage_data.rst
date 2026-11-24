==========================================
Manage Data in Database and on Local Disk
==========================================

.. toctree:: 
  :maxdepth: 3

.. contents:: Index
  :local: 

This module is used to display the four main types of data that may exist for the user in the database and/or on local disk. They are: (1) Raw experiment data; (2) Edited experiment data; (3) Model; and (4) Noise. Each of those hangs off an experiment, which is the top level of the tree. Besides presenting a tree view of the user's data and their relationships, this module provides a simple means of performing processing on data: (1) upload to DB; (2) download to local disk; (3) remove from DB or local. 

.. note::

   A row that both the database and local disk hold reads **Unverified**
   (teal) until the two copies have actually been compared; it then turns
   **In Sync** (green) or **Conflict** (red). A copy that could not be read
   for the comparison counts as a conflict, not as in sync. Comparing means
   hashing the record contents on both sides, which for a multi-wavelength
   run is the slowest part of a scan by a wide margin, so it is done after
   the tree is complete and only for the experiments that exist in both
   places -- a record that is in one place only has nothing to be compared
   against. Opening an experiment moves it to the front of the comparison
   queue -- also while the scan is still reading the records, in which case
   it is compared first when the comparisons start; outside a scan the
   comparison runs in the background, one experiment at a time between other
   events, so opening rows does not wait for it. An experiment row reads
   what its records read, as the tree shows them: **Conflict** when any
   record under it does, **Unverified** while any of them has not been
   compared, and **In Sync** otherwise. A record you upload or download is
   compared again, on its own, straight afterwards; until then it and its
   experiment read **Unverified**. A model or noise record is compared with
   its line ends made LF in both places: a file written on Windows, with
   CRLF line ends, is in sync with the same record in the database, and so
   is a record the database holds with CRLF line ends -- uploaded from such
   a file -- with a local file that has LF ones. An error that a comparison running in the
   background runs into is reported when the background work is done, as a
   scan's errors are.

The primary end of any processing of the data is to achieve a synchronizing of data in the database and on local disk. Particularly when preparing for off-network work or when returning to the network after such work, it is desirable to insure that any new data produced has a presence on local disk for off-network work and in the database for normal day-to-day processing. Instances of any data need to be in sync on the two media. This module provides a means to accomplish that.   

Data Management Process:
==========================

    * **Database Password:** Upon opening, US_ManageData requires you to sign on to the database with your DB password. 
    * **Investigator:** For database work, you must specify your investigator name so you are limited to data that you have permission to access and modify. When the investigator is changed while the database is being read -- in another window, say -- what was read belongs to the previous one: the scan stops, nothing more of it is read or shown, the tree is cleared and the status line says so; scan again to see the new investigator's data. 
    * **Scan Data:** Initiate a scan of all your data on the database and local disk. The scan runs in three phases: the experiments appear first, each with a count of the records under it; the records below them are then read -- from the database in a handful of queries for the whole store, and from disk in a single pass -- and filled into the tree as they arrive; finally the experiments that exist in *both* the database and on local disk are compared record by record, which is what turns a row red when the two copies have drifted apart. With a run selected in **RunID**, only that run is read, one query per kind of record, instead of the whole store; the database's run IDs are matched without case, as the database never tells them apart, and the local ones as the local disk compares file names -- and a local run is always shown with the database run it pairs with, so a run is never shown as in the database only while its directory is there. The local disk is read on a thread of its own, so that walking thousands of files happens at the same time as the database queries rather than after them. The window takes input between experiments: while a database query runs it does not respond, and while the local store is listed and, without a run selected, read as a whole, it is redrawn but ignores the mouse and keyboard. With a run selected, that run's local files are read in the window itself, which is not redrawn until they are. An experiment one of the two places cannot read is read once more, which helps when what made it fail passes -- the server waited too long for a lock, say -- and if that fails too it is left out of the tree and the error is reported: showing what the other place holds would make its records look as if they were only there. Opening the experiment later tries again. A lost database connection is different: the window does not connect again, so the read is not repeated, every later database read fails too, and the message says so -- close the window and open it again. Between experiments you can open experiments in the tree while the rest of it is still being read; the buttons, the filters, **Close** and the row context menus are disabled until the scan is done, and closing the window from its title bar stops the scan and closes it without the reports below. Two progress bars say where a scan is: the upper one (**Overall**) counts the steps of the phase that is running -- listing, reading, comparing -- and the lower one (**This experiment**) the work inside the experiment being read or compared. When the scan is done, any error it ran into is reported -- in particular, if the database could not be listed, every record would otherwise appear to be on local disk only -- and records that share a GUID with another record, in any experiment, are reported once. A record held once in the database and once on local disk is two copies of one record, not a duplicate; a duplicate is a GUID that the database, or the local disk, holds more than once. A local results directory that does not exist yet is an empty store, not an error.
    * **Navigate and Process:** Once you have a tree view of your data, you may navigate it using expand/collapse buttons or the normal **+** and **-** mechanism for specific branch expand and collapse. Opening an experiment the scan has not reached yet moves it to the front of the queue -- both for reading it and for comparing it -- so what you are looking at is dealt with first. Context menus at each row allow upload/download/remove/details, and acting on an experiment acts on everything under it, and only on that. When nothing selected can take an action (removing from the database records that are only on local disk, say) you are told so rather than shown a success. After an action, the colors, the *Children* and *Descendants* counts and the summary on the left show where the records are now -- in every experiment the action reached, not only the one acted on -- and they stay so when an experiment is shown again because a later read hung records under it. What an action did is shown on that record only: a copy of it with the same GUID -- in a run directory copied under another name, or a second local file of it -- is another record. What the database removed along with a record -- an edit's models and their noise, and noise computed from the edit that is shown under another experiment -- is not shown again when an experiment the scan had not reached yet is read later. While an action asks its question and runs, the window's background reads and comparisons wait, so the action works on the records the question was about; they are done after it. Opening a context menu on a selection that holds experiments not read yet reads them first; the menu then acts on the selection as it is after that read. A removal the database refuses is reported with what the database said, and the record stays where it was -- and so does everything under it: what was selected under a record the database kept is not removed record by record instead, since that would leave just the part of it the database refused to remove. Parents are removed before their descendants, and the message lists the selected records that were removed and those that were not. The sample tree shown before the first scan is not your data, and no action can be taken on it.
    * **Upload and download:** A record is uploaded only when its parent is in the database, and downloaded only when its parent is on local disk; acting on the parent takes its descendants along, top down, so selecting the edit downloads the edit and then its models. Uploading a record the database already holds (a **DB replace**) first compares the two copies, hashing the local file again in case it changed since the last comparison: when they hold the same thing, nothing is written and you are told the record is already in sync -- whatever the dates say, since the database copy may well have come from that very file. Only when the copies differ is a local copy that is older than the database copy refused; both dates are read as UTC. A model or noise record is uploaded from the file its row was read from; when that file cannot be read, nothing is uploaded and you are told so. A downloaded model is written exactly as the database holds it, in UTF-8. A model or noise record written to the database whose database ID cannot be read back is shown in the database, but cannot be removed or downloaded until the next scan reads its ID.
    * **Removing an edit from the database:** The database removes an edit's models and their noise with it, in one transaction. Noise of a model of *another* edit that was computed from this edit (a global fit, say) is also recorded against the edit, and goes with it too: the question asked before an edit is removed says so. Your own such noise is removed with the edit -- its rows, under that other model and in whatever experiment it is, then show it gone from the database -- and it is checked against the GMP framework as if you had selected it: when its model was made by GMP, or its run is one GMP needs, nothing is removed. Such noise of *another investigator's* model, which you cannot see, is never removed without a word. A current database refuses to remove the edit while it would take along records of another investigator or records the GMP framework refers to (an administrator may remove another investigator's), and changes nothing; an older database refuses it while that noise refers to the edit, by then having removed the edit's own models and noise, which the transaction gives back. An older database also refuses the edit while your own such noise refers to it: then that noise is removed first, in a second transaction, and the edit asked for again. Either way you are told why with the database's message -- only that investigator, or an administrator, can remove their noise -- and nothing is removed: neither the edit nor what was selected under it. Only that refusal of an older database is tried again. When another session holds records the removal needs for longer than the database waits (a lock wait timeout), or the database breaks off the removal to resolve a deadlock with another session, nothing is changed, it is not tried again, and you are told to try again later. A failure the transaction gave back as a whole changed nothing, and the message says so. When a removal fails and is not given back as a whole, or its outcome is not confirmed, what the database holds is read back: a record it no longer holds is shown gone, and the message names any record that was removed all the same. So is an edit the database says it does not have (another session removed it): it and what went with it are then shown gone. When the connection to the database is lost, whether the removal happened is not known; since the window does not connect again, nothing can be read back and every later action fails too, and the message says to close the window, open it again and look at those records before acting on them.
    * **Experiments:** The top level of the tree is one row per experiment, labelled with its run ID. Two database experiments may share a run ID; a local run is then paired with the one whose GUID -- or else whose experiment ID -- its experiment file records, and only failing both with the first one left. The GUID matches are made first, over all the local runs, so a run that is only paired by order never takes the experiment another run records. An experiment ID means something only in the database it came from: a local run whose file records a GUID that none of the experiments has came from another database, and is not paired by its ID. Before an experiment has been read, its *Descendants* column shows how many records the listing says are under it ("?" when the source cannot say without reading them); once read, it shows what is actually in the tree. The summary on the left counts experiments without the stand-in rows described below.
    * **Orphans:** A record whose parent is missing (a model whose edit was removed, or an edit whose raw data is gone) is shown at the end of the tree under a stand-in experiment, **(records of no experiment)**, below gray **dummy** filler rows that stand in for its missing parents, so that it can still be seen and removed. Raw data whose experiment is missing, or is not yours, is shown too but cannot be removed here: the database removes raw data only together with its experiment. Global models, which have no edit by design, are under a stand-in of their own, **(global models)**; a global model one place records with no edit GUID and the other with the all-zero one is one record in both places. The **Source** filters apply to these records as to any others: with **Exclude DB-Only Trees**, an orphan that is only in the database is not shown. A stand-in row is no record itself: its *Source* reads **--**, and the records under it that are in both places are compared like any others. Removing from a stand-in row removes everything under it, so you are asked again, with the number of records that would go. A record whose parent is in the other place only -- a model on local disk whose edit is only in the database, say -- is not an orphan: it is shown under that parent, paired with its other copy. Only a scan of the whole store can tell a missing parent from one that was not read, so orphans are not shown when the tree is narrowed to one run or one triple.
    * **Runs with problems:** A run that should not be acted on as it stands is marked **(!)** after its run ID; holding the mouse over it says why, and the problems are listed when the scan is done. A run ID with dots in it is from an older version of UltraScan (new run IDs may use only letters, digits, ``_`` and ``-``); such runs are still shown and can be acted on. Other problems are a run directory not named after the run its data files name, data files of more than one run in one directory, and two directories holding the same run. Nothing is uploaded or downloaded for a run whose directory is not named after it or is not the only one holding it -- what would be written would not go where the run is -- until its directory is put right; its records can still be removed. Such a run is always marked **(!)**. Whether a directory is named after its run is decided as the file system decides it: where file names ignore case (Windows, and macOS as installed), ``runa`` is the directory of run ``RunA``. A directory that also holds the files of another run is fine for the run it is named after.


Initially you are presented with a small window to enter your database password. Then you will see the main window with a sample data tree that shows the kinds of states that data may be in (see tree help below).

.. image:: _static/images/manage_data.png
    :align: center

.. rst-class:: 
    :align: center

    **Data Main Window**

 After clicking on the **Scan Data** button, the tree will be populated with your actual data. 

.. image:: _static/images/manage_data_exp.png
    :align: center

.. rst-class:: 
    :align: center

    **Populated Data Manager**

.. _manage-data-thelp:

The tree menu will expand and collapse with buttons like these.

.. grid:: 2
  :gutter: 2 

  .. grid-item:: 

    .. image:: _static/images/manage_data_bshow.png
      :align: left
      :width: 100%  
  
  .. grid-item:: 

    .. image:: _static/images/manage_data_bhide.png
      :width: 100%
      :align: right

.. rst-class:: center

    **Tree Menu Buttons**

.. _manage-data-cmenu:

.. image:: _static/images/cmenu_help.png
  :align: center

Data Management Functions:
============================

.. list-table::
  :widths: 20 50
  :header-rows: 0

  * - **Investigator:**
    - Often the investigator text field will already be correctly filled out if your home directory name is the same as the first or last of your investigator name. If not, you may enter all or a portion of the first or last name and hit the **Enter** key to have the investigator found. If that, too, fails, you may click the button to enter a full :doc:`Investigator dialog <us_investigator>`. Choosing another investigator resets the window, as **Reset** does: what was shown was the previous person's, so scan again.
  * - **Reset**
    - Reset the tree to the default sample data.
  * - **Scan Data**
    - Click this button to initiate a full scan of all your data in the database and on local disk. The list of experiments appears straight away; the records under each experiment are read afterwards and appear as they are read; and the experiments held in both places are then compared. Records you upload or download are compared again by themselves; re-initiate a scan to see changes made outside this window.
  * - **Show All Edits**
    - Expand the tree view to insure all rows at the level of Edited data are revealed. The button will then be relabelled **Collapse All** so you can hide edits and their descendants.
  * - **Show All Models**
    - Expand the tree view to insure all rows at the level of Model data are revealed. The button will then be relabelled **Hide All Models** so you can hide models and their descendants.
  * - **Expand All**  
    - Expand the tree view to insure all rows at the level of Noise data and their ancestors are revealed. The button will then be relabelled **Hide All Noises** so you can hide noise record rows.
  * - **Data Tree Help**
    - This button lets you pop up a :ref:`Tree Help Text <manage-data-thelp>` Window with some helpful notes on use of the tree view and on the color legend for its rows.
  * - **Tree Navigation and Context Menus**
    - Individual rows in the tree view may be expanded ("+") to show children and other descendants or may be collapsed ("-"). A right-mouse-button click on any row brings up a :ref:`Manage Data Context Menu <manage-data-cmenu>` that allows you to perform processes on the data record or show details about it.
  * - **Help**
    - Display this and other documentation.
  * - **Close**
    - Close all windows and exit. Disabled while a scan runs; closing the window from its title bar stops the scan first, or the reading or comparing going on in the background, and the window closes once that has stopped.



