@echo off
REM Windows build script with MSIX packaging support
REM This script handles proper dependency order: qwtplot3d -> utils -> gui -> programs

echo Starting UltraScan3 Windows build with MSIX packaging...

REM Check if we have a local.pri file
if not exist "local.pri" (
    echo Creating local.pri from template...
    copy "local.pri.template" "local.pri"
)

REM Build in correct dependency order
echo Building qwtplot3d library...
cd qwtplot3d
qmake qwtplot3d.pro
if errorlevel 1 (
    echo Failed to configure qwtplot3d
    exit /b 1
)
nmake release
if errorlevel 1 (
    echo Failed to build qwtplot3d
    exit /b 1
)
cd ..

echo Building utils library...
cd utils
qmake
if errorlevel 1 (
    echo Failed to configure utils
    exit /b 1
)
nmake release
if errorlevel 1 (
    echo Failed to build utils
    exit /b 1
)
cd ..

echo Building GUI library...
cd gui
qmake libus_gui.pro
if errorlevel 1 (
    echo Failed to configure GUI
    exit /b 1
)
nmake release
if errorlevel 1 (
    echo Failed to build GUI
    exit /b 1
)
cd ..

echo Building main programs...
cd programs\us
if exist "revision.sh" (
    bash revision.sh
)
qmake us.pro
if errorlevel 1 (
    echo Failed to configure main program
    exit /b 1
)
nmake release
if errorlevel 1 (
    echo Failed to build main program
    exit /b 1
)
cd ..\..

echo Building additional programs...
for /d %%d in (programs\us_*) do (
    if exist "%%d\*.pro" (
        echo Building %%d...
        cd %%d
        qmake *.pro
        if not errorlevel 1 (
            nmake release
        )
        cd ..\..
    )
)

echo Build completed. Checking for executables...
if not exist "bin\us.exe" (
    echo Error: Main executable not found. Build may have failed.
    exit /b 1
)

REM Copy libraries to bin directory
echo Copying libraries to bin directory...
if exist "lib\*.dll" (
    copy "lib\*.dll" "bin\"
)

REM Ensure we have the required directories for packaging
if not exist "C:\dist" (
    echo Creating distribution directory...
    mkdir "C:\dist"
    mkdir "C:\dist\bin"
    mkdir "C:\dist\etc"
    mkdir "C:\dist\lib"
    
    REM Copy essential files
    echo Copying application files...
    xcopy /E /Y "bin\*" "C:\dist\bin\"
    xcopy /E /Y "etc\*" "C:\dist\etc\"
    xcopy /E /Y "lib\*" "C:\dist\lib\"
    copy /Y "LICENSE.txt" "C:\dist\"
)

REM Create MSIX package
echo Creating MSIX package...
powershell -ExecutionPolicy Bypass -File "packaging\create-msix-package.ps1"
if errorlevel 1 (
    echo MSIX packaging failed
    exit /b 1
)

echo Build and packaging completed successfully!
echo MSIX package should be available in the current directory.