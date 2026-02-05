# UltraScan Data Publication

The `us_data_publication` tool enables exporting and importing data publication bundles (`.tar.gz`) for UltraScan3 data. This allows researchers to share complete datasets with all dependencies, transfer data between installations, and create backups of research data.

## Overview

The tool supports:
- **Export**: Package projects, experiments, raw data, and analysis results into a portable bundle
- **Import**: Load data bundles into an UltraScan3 database or local disk storage
- Both **CLI** and **GUI** interfaces

## GUI Interface

The GUI is organized into two tabs: **Export** and **Import**.

### Export Tab

1. **Data Source**: Select whether to export from Database or Local Disk using the radio buttons at the top.

2. **Project Selection** (optional): Click "Select Project..." to open the project selection dialog. Only one project can be selected at a time.

3. **Experiment Selection**: Click "Select Runs..." to open the run selection dialog. You can select multiple experiments/runs. If a project was selected, you can prefilter by that project.

4. **Data Tree**: After selecting experiments, a tree view displays:
   - Experiments at the top level
   - Raw Data as children of experiments
   - Edits as children of raw data
   
   Each item has a checkbox for selection.

5. **Bulk Actions for Data**:
   - **Select All Raw**: Selects all raw data entries
   - **Deselect All Raw**: Deselects all raw data entries
   - **Latest Edits**: For each selected raw data, selects only the latest edit
   - **All Edits**: Selects all edits for selected raw data
   - **No Edits**: Deselects all edits

6. **Model Selection**: Click "Select Models..." to open the model selection dialog (filtered by selected runs). Selected models appear in a separate tree view.

7. **Dependency Handling**: When selecting models, the tool checks if required edits are selected. If not, it prompts you to either:
   - Auto-select the required edit, or
   - Deselect the model

8. **Summary**: Shows counts of selected raw data, edits, and models. Use "Preview Manifest..." to see a YAML preview of the bundle contents.

9. **Export**: Specify the output file path and click "Export Bundle" to create the bundle.

### Import Tab

1. **Target**: Select whether to import to Database or Local Disk.

2. **Bundle File**: Browse to select a `.tar.gz` bundle file to import.

3. **Conflict Policy**:
   - **Reuse if properties match**: If an entity with matching properties exists, reuse it
   - **Always rename**: Create new entities with unique names
   - **Fail**: Stop on any conflict

4. **Preview**: Shows the contents of the selected bundle.

5. **Import**: Click "Import Bundle" to start the import process.

## Supported Entity Types

Data bundles can include the following entity types, organized by their dependency hierarchy:

Data bundles can include the following entity types, organized by their dependency hierarchy:

1. **Project** - Top-level container for related experiments
2. **Experiment** - Individual experimental runs
3. **RawData** - Original AUC data files
4. **RotorCalibration** - Rotor calibration profiles
5. **Centerpiece** - Centerpiece definitions
6. **Buffer** - Buffer compositions
7. **Analyte** - Analyte definitions (proteins, DNA, RNA, etc.)
8. **Solution** - Solution compositions (buffer + analytes)
9. **Edit** - Data edit profiles
10. **Model** - Analysis models
11. **Noise** - Noise vectors (TI and RI)

## Bundle Format

### Structure

A data publication bundle is a `.tar.gz` archive containing:

```
bundle.tar.gz
├── manifest.yaml          # Bundle metadata and entity list
├── project/               # Project XML files
│   └── {guid}.xml
├── experiment/            # Experiment data
│   └── {guid}.xml
├── rawData/               # Raw AUC data files
│   └── {guid}.auc
├── buffer/                # Buffer definitions
│   └── {guid}.xml
├── analyte/               # Analyte definitions
│   └── {guid}.xml
├── solution/              # Solution definitions
│   └── {guid}.xml
├── edit/                  # Edit profiles
│   └── {guid}.xml
├── model/                 # Analysis models
│   └── {guid}.xml
├── noise/                 # Noise vectors
│   └── {guid}.xml
└── rotorCalibration/      # Rotor calibrations
    └── {id}.xml
```

### Manifest Format (YAML)

```yaml
# UltraScan3 Data Publication Bundle Manifest
version: "1.0"
created: "2024-01-15T10:30:00"
description: "Example research data bundle"

project:
  - id: 123
    guid: "a1b2c3d4-e5f6-..."
    name: "My Research Project"
    propertyHash: "sha256:..."
    payloadPath: "project/a1b2c3d4-e5f6-....xml"

experiment:
  - id: 456
    guid: "b2c3d4e5-f6a7-..."
    name: "Experiment 1"
    propertyHash: "sha256:..."
    payloadPath: "experiment/b2c3d4e5-f6a7-....xml"
    dependencies:
      - "a1b2c3d4-e5f6-..."  # project GUID

buffer:
  - id: 789
    guid: "c3d4e5f6-a7b8-..."
    name: "PBS Buffer"
    propertyHash: "sha256:..."
    payloadPath: "buffer/c3d4e5f6-a7b8-....xml"
# ... additional entity types
```

## CLI Usage

### Export

```bash
# Export entire project
us_data_publication --mode export --bundle output.tar.gz --project-id 123

# Export specific experiment
us_data_publication --mode export --bundle output.tar.gz \
    --project-id 123 --experiment-id 456

# Export with limited scope (only up to edits, no models/noise)
us_data_publication --mode export --bundle output.tar.gz \
    --project-id 123 --max-scope edits

# Verbose output with dry-run
us_data_publication --mode export --bundle output.tar.gz \
    --project-id 123 --verbose --dry-run
```

### Import

```bash
# Import to database
us_data_publication --mode import --bundle data.tar.gz --target db

# Import to local disk
us_data_publication --mode import --bundle data.tar.gz \
    --target disk --output-dir /path/to/output

# Import with specific conflict policy
us_data_publication --mode import --bundle data.tar.gz \
    --on-conflict reuse   # or: rename, fail

# Non-interactive import
us_data_publication --mode import --bundle data.tar.gz \
    --non-interactive --on-conflict reuse
```

### CLI Options

| Option | Description |
|--------|-------------|
| `--mode <export\|import>` | Operation mode |
| `--bundle <path>` | Path to bundle file (.tar.gz) |
| `--project-id <id>` | Project ID for export |
| `--experiment-id <id>` | Experiment ID for export |
| `--max-scope <scope>` | Export scope: project, experiment, rawdata, edits, models, all |
| `--target <db\|disk>` | Import target (default: db) |
| `--output-dir <path>` | Output directory for disk import |
| `--on-conflict <policy>` | Conflict policy: reuse, rename, fail |
| `--non-interactive` | Run without user prompts |
| `--verbose` | Enable verbose output |
| `--dry-run` | Show what would be done without making changes |
| `--no-ui` | Force CLI mode |
| `--help` | Show help |

## GUI Usage

Launch the GUI by running `us_data_publication` without any arguments:

```bash
us_data_publication
```

### Export Wizard

1. Select **Export** mode
2. Click **Select** to choose a project
3. Optionally select a specific experiment
4. Choose the export scope from the dropdown
5. Click **Browse** to select the output file location
6. Click **Start** to begin export

### Import Wizard

1. Select **Import** mode
2. Choose the target (Database or Disk)
3. Select conflict resolution policy
4. Click **Browse** to select the bundle file
5. Click **Start** to begin import
6. Review any conflicts in the dialog and choose resolution

## Conflict Handling

When importing, conflicts may occur if entities with the same name already exist.

### Conflict Policies

| Policy | Behavior |
|--------|----------|
| **Reuse** | If properties match, reuse existing entity; if different, auto-rename |
| **Rename** | Always create new entity with unique name suffix |
| **Fail** | Stop import on any conflict |

### Property Matching

Two entities are considered matching if their property hash (SHA-256) is identical. The hash is computed from:
- Entity name/description
- Key scientific properties (e.g., density, viscosity for buffers)
- Configuration parameters

### ID Remapping

During import:
- Database IDs are **never** preserved (assigned by target DB)
- GUIDs are regenerated for new entities
- A GUID mapping table maintains relationships between imported entities
- Dependencies are automatically updated to reference new IDs

## Examples

### Research Data Sharing

```bash
# Researcher A: Export complete project
us_data_publication --mode export \
    --bundle my_research.tar.gz \
    --project-id 42 \
    --verbose

# Researcher B: Import to their database
us_data_publication --mode import \
    --bundle my_research.tar.gz \
    --target db \
    --on-conflict reuse
```

### Backup and Restore

```bash
# Create backup
us_data_publication --mode export \
    --bundle backup_2024-01.tar.gz \
    --project-id 1 \
    --max-scope all

# Restore to fresh database
us_data_publication --mode import \
    --bundle backup_2024-01.tar.gz \
    --target db \
    --on-conflict fail  # Fail if duplicates exist
```

### Data Migration

```bash
# Export from source system
us_data_publication --mode export \
    --bundle migration.tar.gz \
    --project-id 100

# Import to new system with renaming
us_data_publication --mode import \
    --bundle migration.tar.gz \
    --target db \
    --on-conflict rename  # Force unique names
```

## Error Handling

The tool provides detailed error messages for common issues:

- **Missing dependencies**: Entity requires a dependency not in the bundle
- **Invalid bundle format**: Archive corrupted or missing manifest
- **Database connection failed**: Cannot connect to UltraScan database
- **Permission denied**: Cannot write to output directory
- **Conflict resolution failed**: Cannot resolve naming conflict

## Technical Notes

### Dependencies

The tool uses existing UltraScan3 utilities:
- `us_archive.h` - Archive compression/extraction
- `us_db2.h` - Database operations
- `us_project.h`, `us_buffer.h`, etc. - Entity serialization

### Thread Safety

Export and import operations run in the main thread but emit progress signals for GUI updates. For large bundles, consider running CLI mode with `--verbose` to monitor progress.

### Limitations

- Maximum bundle size limited by available disk space
- Import does not support merging existing entities (reuse or create new only)
- Centerpiece definitions may require manual verification after import
