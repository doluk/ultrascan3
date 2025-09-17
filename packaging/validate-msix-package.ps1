# PowerShell script to validate MSIX package creation
# This script performs basic validation of the created MSIX package

param(
    [string]$PackagePath = "UltraScan3-3.6.0.0.msix"
)

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

Write-ColorOutput Green "Validating MSIX package: $PackagePath"

# Check if package file exists
if (-not (Test-Path $PackagePath)) {
    Write-ColorOutput Red "Error: Package file not found: $PackagePath"
    exit 1
}

# Get package information
$PackageInfo = Get-Item $PackagePath
Write-ColorOutput Cyan "Package File Information:"
Write-ColorOutput White "  Name: $($PackageInfo.Name)"
Write-ColorOutput White "  Size: $([math]::Round($PackageInfo.Length / 1MB, 2)) MB"
Write-ColorOutput White "  Created: $($PackageInfo.CreationTime)"

# Validate package structure using makeappx
Write-ColorOutput Yellow "Validating package structure..."
try {
    $TempDir = Join-Path $env:TEMP "msix-validation"
    if (Test-Path $TempDir) {
        Remove-Item -Recurse -Force $TempDir
    }
    
    # Extract package for validation
    & makeappx unpack /p $PackagePath /d $TempDir /overwrite
    if ($LASTEXITCODE -eq 0) {
        Write-ColorOutput Green "Package structure is valid"
        
        # Check for required files
        $RequiredFiles = @(
            "Package.appxmanifest",
            "UltraScan3\bin\us.exe",
            "Assets\Square44x44Logo.png",
            "Assets\Square150x150Logo.png"
        )
        
        Write-ColorOutput Cyan "Checking required files:"
        $AllFilesPresent = $true
        foreach ($File in $RequiredFiles) {
            $FilePath = Join-Path $TempDir $File
            if (Test-Path $FilePath) {
                Write-ColorOutput Green "  ✓ $File"
            } else {
                Write-ColorOutput Red "  ✗ $File (missing)"
                $AllFilesPresent = $false
            }
        }
        
        if ($AllFilesPresent) {
            Write-ColorOutput Green "All required files are present"
        } else {
            Write-ColorOutput Yellow "Some required files are missing"
        }
        
        # Check manifest content
        $ManifestPath = Join-Path $TempDir "Package.appxmanifest"
        if (Test-Path $ManifestPath) {
            $Manifest = [xml](Get-Content $ManifestPath)
            Write-ColorOutput Cyan "Package Manifest Information:"
            Write-ColorOutput White "  Identity: $($Manifest.Package.Identity.Name)"
            Write-ColorOutput White "  Version: $($Manifest.Package.Identity.Version)"
            Write-ColorOutput White "  Publisher: $($Manifest.Package.Identity.Publisher)"
            Write-ColorOutput White "  Display Name: $($Manifest.Package.Properties.DisplayName)"
        }
        
        # Clean up
        Remove-Item -Recurse -Force $TempDir
        
    } else {
        Write-ColorOutput Red "Package validation failed with exit code: $LASTEXITCODE"
        exit 1
    }
} catch {
    Write-ColorOutput Red "Error during validation: $_"
    exit 1
}

Write-ColorOutput Green "Package validation completed successfully!"

# Additional checks
Write-ColorOutput Cyan "Additional Information:"
Write-ColorOutput White "To install this package, run:"
Write-ColorOutput White "  Add-AppxPackage -Path '$PackagePath'"
Write-ColorOutput White ""
Write-ColorOutput White "Note: You may need to enable Developer Mode or use a signed package."