# UltraScan3 MSIX Packaging

This directory contains the MSIX packaging workflow for creating Windows App Package (MSIX) installers for UltraScan3.

## Overview

MSIX is the modern Windows packaging format that provides:
- Clean installation and uninstallation
- Automatic updates
- Security sandboxing
- Microsoft Store compatibility
- Side-loading support for enterprise deployment

## Files

### Core Scripts
- `create-msix-package.ps1` - PowerShell script that creates the MSIX package
- `Package.appxmanifest.template` - Template for the MSIX manifest file
- `buildwin-msix.cmd` - Integrated build script that includes MSIX packaging

### Workflow
- `.github/workflows/windows-msix.yml` - GitHub Actions workflow for automated building

## Usage

### Local Development

1. **Prerequisites:**
   - Windows 10/11 with Windows SDK
   - Qt 5.15+ installed
   - Visual Studio Build Tools or Visual Studio
   - PowerShell 5.0+

2. **Build and Package:**
   ```cmd
   # Complete build and MSIX packaging
   buildwin-msix.cmd
   
   # Or manually after building:
   powershell -ExecutionPolicy Bypass -File packaging\create-msix-package.ps1
   ```

3. **Install Package:**
   ```powershell
   # Install the MSIX package (requires Developer Mode or signed package)
   Add-AppxPackage -Path "UltraScan3-3.6.0.0.msix"
   ```

### GitHub Actions

The automated workflow builds and packages UltraScan3 on every push to main/master branches:

1. Sets up Qt and build environment
2. Compiles the application
3. Creates MSIX package
4. Uploads as GitHub artifact

## Customization

### Version Numbers
Update the version in:
- `Package.appxmanifest.template` - `Identity/@Version`
- `create-msix-package.ps1` - `$Version` parameter default

### Application Identity
Modify in `Package.appxmanifest.template`:
- `Identity/@Name` - Unique package name
- `Identity/@Publisher` - Publisher certificate subject
- `Properties/DisplayName` - App display name

### Icons and Assets
Place required icons in the `etc/` directory:
- `us3-icon-48x48.png` → Square44x44Logo.png
- `us3-icon-128x128.png` → Square150x150Logo.png

Additional assets can be created:
- `Wide310x150Logo.png` (310×150)
- `SplashScreen.png` (620×300)
- `StoreLogo.png` (50×50)

## Deployment

### Side-loading (Enterprise)
1. Enable Developer Mode or use signed packages
2. Install using `Add-AppxPackage` PowerShell cmdlet
3. Or double-click the .msix file

### Microsoft Store
1. Create App Center or Partner Center account
2. Upload signed MSIX package
3. Complete store submission process

### Group Policy Deployment
Use Windows group policy to deploy MSIX packages across enterprise networks.

## Troubleshooting

### Common Issues

1. **"App package must be signed"**
   - Enable Developer Mode in Windows Settings
   - Or sign the package with a trusted certificate

2. **Missing dependencies**
   - Ensure Qt DLLs are included in the package
   - Use `windeployqt` to gather dependencies

3. **Build failures**
   - Check that all paths in the scripts match your environment
   - Verify Qt and build tools are properly installed

### Debug Information

View detailed package information:
```powershell
Get-AppxPackage | Where-Object {$_.Name -like "*UltraScan*"}
```

Check installation logs in Event Viewer under:
`Applications and Services Logs > Microsoft > Windows > AppXDeployment-Server`

## Notes

- MSIX packages are forward-compatible but not backward-compatible
- Minimum Windows version is 10.0.17763.0 (Windows 10 version 1809)
- The package uses `runFullTrust` capability for full system access
- File associations are configured for `.auc` and `.us3` files