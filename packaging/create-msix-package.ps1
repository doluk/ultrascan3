# PowerShell script to create MSIX package for UltraScan3
# This script creates an MSIX package from the built application files

param(
    [string]$SourceDir = "C:\dist",
    [string]$PackageDir = ".\msix-package",
    [string]$OutputDir = ".",
    [string]$Version = "3.6.0.0"
)

# Function to write colored output
function Write-ColorOutput($ForegroundColor) {
    $fc = $host.UI.RawUI.ForegroundColor
    $host.UI.RawUI.ForegroundColor = $ForegroundColor
    if ($args) {
        Write-Output $args
    } else {
        $input | Write-Output
    }
    $host.UI.RawUI.ForegroundColor = $fc
}

Write-ColorOutput Green "Starting MSIX package creation for UltraScan3..."

# Clean and create package directory
if (Test-Path $PackageDir) {
    Remove-Item -Recurse -Force $PackageDir
}
New-Item -ItemType Directory -Force -Path $PackageDir | Out-Null

# Create the package structure
$AppDir = Join-Path $PackageDir "UltraScan3"
New-Item -ItemType Directory -Force -Path $AppDir | Out-Null

Write-ColorOutput Yellow "Copying application files..."

# Copy application files
if (Test-Path $SourceDir) {
    Copy-Item -Recurse -Force "$SourceDir\*" $AppDir
} else {
    Write-ColorOutput Red "Source directory $SourceDir not found!"
    exit 1
}

# Create Assets directory for MSIX
$AssetsDir = Join-Path $PackageDir "Assets"
New-Item -ItemType Directory -Force -Path $AssetsDir | Out-Null

Write-ColorOutput Yellow "Creating MSIX assets..."

# Copy and prepare icons from etc directory
$EtcDir = Join-Path $AppDir "etc"
if (Test-Path $EtcDir) {
    # Copy icons to Assets directory with MSIX naming convention
    if (Test-Path "$EtcDir\us3-icon-48x48.png") {
        Copy-Item "$EtcDir\us3-icon-48x48.png" "$AssetsDir\Square44x44Logo.png"
    }
    if (Test-Path "$EtcDir\us3-icon-128x128.png") {
        Copy-Item "$EtcDir\us3-icon-128x128.png" "$AssetsDir\Square150x150Logo.png"
    }
    # Create additional required assets if they don't exist
    if (-not (Test-Path "$AssetsDir\StoreLogo.png")) {
        Copy-Item "$EtcDir\us3-icon-48x48.png" "$AssetsDir\StoreLogo.png" -ErrorAction SilentlyContinue
    }
}

Write-ColorOutput Yellow "Creating Package.appxmanifest..."

# Create the manifest file
$ManifestPath = Join-Path $PackageDir "Package.appxmanifest"
$Manifest = @"
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities">
  <Identity Name="UltraScan3.App"
            Publisher="CN=UltraScan3"
            Version="$Version" />
  <Properties>
    <DisplayName>UltraScan3</DisplayName>
    <PublisherDisplayName>UltraScan3 Project</PublisherDisplayName>
    <Logo>Assets\StoreLogo.png</Logo>
    <Description>UltraScan3 - Analytical Ultracentrifugation Data Analysis Software</Description>
  </Properties>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.22621.0" />
  </Dependencies>
  <Capabilities>
    <rescap:Capability Name="runFullTrust" />
  </Capabilities>
  <Applications>
    <Application Id="UltraScan3" Executable="UltraScan3\bin\us.exe" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="UltraScan3"
                          Square150x150Logo="Assets\Square150x150Logo.png"
                          Square44x44Logo="Assets\Square44x44Logo.png"
                          Description="UltraScan3 - Analytical Ultracentrifugation Data Analysis Software"
                          BackgroundColor="transparent">
      </uap:VisualElements>
    </Application>
  </Applications>
</Package>
"@

$Manifest | Out-File -FilePath $ManifestPath -Encoding UTF8

Write-ColorOutput Yellow "Building MSIX package..."

# Build the MSIX package
$PackageName = "UltraScan3-$Version.msix"
$OutputPath = Join-Path $OutputDir $PackageName

try {
    & makeappx pack /d $PackageDir /p $OutputPath /overwrite
    if ($LASTEXITCODE -eq 0) {
        Write-ColorOutput Green "MSIX package created successfully: $OutputPath"
        
        # Display package info
        Write-ColorOutput Cyan "Package Information:"
        Write-ColorOutput White "  Name: $PackageName"
        Write-ColorOutput White "  Size: $((Get-Item $OutputPath).Length / 1MB) MB"
        Write-ColorOutput White "  Location: $OutputPath"
    } else {
        Write-ColorOutput Red "Failed to create MSIX package. Exit code: $LASTEXITCODE"
        exit 1
    }
} catch {
    Write-ColorOutput Red "Error creating MSIX package: $_"
    exit 1
}

Write-ColorOutput Green "MSIX package creation completed successfully!"