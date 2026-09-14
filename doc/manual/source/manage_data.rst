==========================================
Manage Data in Database and on Local Disk
==========================================

.. toctree:: 
  :maxdepth: 3

.. contents:: Index
  :local: 

This module is used to display the four main types of data that may exist for the user in the database and/or on local disk. They are: (1) Raw experiment data; (2) Edited experiment data; (3) Model; and (4) Noise. Each of those hangs off an experiment, which is the top level of the tree. Besides presenting a tree view of the user's data and their relationships, this module provides a simple means of performing processing on data: (1) upload to DB; (2) download to local disk; (3) remove from DB or local. 

.. note::

   A row reads as **In Sync** as soon as both the database and local disk hold
   that record; it turns **Conflict** only once the two copies have actually
   been compared. Comparing means hashing the record contents on both sides,
   which for a multi-wavelength run is the slowest part of a scan by a wide
   margin, so it is done after the tree is complete and only for the
   experiments that exist in both places -- a record that is in one place only
   has nothing to be compared against. Opening an experiment compares it
   straight away.

The primary end of any processing of the data is to achieve a synchronizing of data in the database and on local disk. Particularly when preparing for off-network work or when returning to the network after such work, it is desirable to insure that any new data produced has a presence on local disk for off-network work and in the database for normal day-to-day processing. Instances of any data need to be in sync on the two media. This module provides a means to accomplish that.   

Data Management Process:
==========================

    * **Database Password:** Upon opening, US_ManageData requires you to sign on to the database with your DB password. 
    * **Investigator:** For database work, you must specify your investigator name so you are limited to data that you have permission to access and modify. 
    * **Scan Data:** Initiate a scan of all your data on the database and local disk. The scan runs in three phases: the experiments appear first, each with a count of the records under it; the records below them are then read -- from the database in a handful of queries for the whole store, and from disk in a single pass -- and filled into the tree as they arrive; finally the experiments that exist in *both* the database and on local disk are compared record by record, which is what turns a row red when the two copies have drifted apart. You can start working with the tree while the rest of it is still being read. Two progress bars say where a scan is: the upper one counts the experiments of the phase that is running, the lower one the work inside the experiment being read.
    * **Navigate and Process:** Once you have a tree view of your data, you may navigate it using expand/collapse buttons or the normal **+** and **-** mechanism for specific branch expand and collapse. Opening an experiment the scan has not reached yet moves it to the front of the queue -- both for reading it and for comparing it -- so what you are looking at is dealt with first. Context menus at each row allow upload/download/remove/details, and acting on an experiment acts on everything under it. 


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
    - Often the investigator text field will already be correctly filled out if your home directory name is the same as the first or last of your investigator name. If not, you may enter all or a portion of the first or last name and hit the **Enter** key to have the investigator found. If that, too, fails, you may click the button to enter a full :doc:`Investigator dialog <us_investigator>`.
  * - **Reset**
    - Reset the tree to the default sample data.
  * - **Scan Data**
    - Click this button to initiate a full scan of all your data in the database and on local disk. The list of experiments appears straight away; the records under each experiment are read afterwards and appear as they are read; and the experiments held in both places are then compared. You should re-initiate a scan after any series of processes (upload|download|remove) on the data.
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
    - Close all windows and exit.


