// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// Version and branding constants for the NØCTE engine. Kept free of any other include so that
// both libslic3r and the GUI layer can pull it in without dragging dependencies along.

#ifndef slic3r_Nocte_NocteVersion_hpp_
#define slic3r_Nocte_NocteVersion_hpp_

// Engine version. Bumped independently of the upstream OrcaSlicer version.
#define NOCTE_VERSION "0.1.0-dev"

// ASCII application name. Used wherever a plain, filesystem- and registry-safe name is needed.
#define NOCTE_APP_NAME "NOCTE Slicer"

// Display name, UTF-8. Used in window titles, the About dialog and reports.
#define NOCTE_APP_DISPLAY_NAME u8"NØCTE Slicer"

// Value written into the 3mf metadata so a NØCTE-produced project can be recognised again.
// Additive only: Bambu Studio ignores unknown Metadata entries (ADR-001).
#define NOCTE_3MF_METADATA_TAG "NocteSlicer"

// Schema identifier of the JSON report produced by Nocte::NocteReport::to_json().
#define NOCTE_REPORT_SCHEMA "nocte.report/1"

#endif // slic3r_Nocte_NocteVersion_hpp_
