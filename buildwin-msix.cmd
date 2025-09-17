@echo off
REM Windows build script with MSIX packaging support
REM This script extends the existing buildwin.cmd with MSIX packaging capability

echo Starting UltraScan3 Windows build with MSIX packaging...

REM First run the existing build process
echo Running existing build process...
call buildwin.cmd
if errorlevel 1 (
    echo Build failed, cannot proceed with packaging
    exit /b 1
)

REM Check if we're in a proper build environment
if not exist "bin\us.exe" (
    echo Error: Main executable not found. Build may have failed.
    exit /b 1
)

REM Run the existing copy packaging script if available
if exist "copypkg-win.sh" (
    echo Running existing packaging script...
    bash copypkg-win.sh
    if errorlevel 1 (
        echo Warning: Existing packaging script failed, continuing with MSIX creation...
    )
)

REM Ensure we have the required directories
if not exist "C:\dist" (
    echo Creating distribution directory...
    mkdir "C:\dist"
    mkdir "C:\dist\bin"
    mkdir "C:\dist\etc"
    
    REM Copy essential files
    echo Copying application files...
    xcopy /E /Y "bin\*" "C:\dist\bin\"
    xcopy /E /Y "etc\*" "C:\dist\etc\"
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