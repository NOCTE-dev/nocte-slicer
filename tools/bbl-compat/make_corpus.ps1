# NØCTE Slicer — regenerate the synthetic corpus/nocte-cli 3MF corpus.
#
# Thin launcher around make_corpus.py (Python does the zip/XML work). Four cases
# are written end-to-end by the NØCTE CLI:
#   01_single_cube  02_modifier_block  03_negative_part  04_support_enforcer_blocker
#
# Usage:
#   powershell -File tools\bbl-compat\make_corpus.ps1
#   powershell -File tools\bbl-compat\make_corpus.ps1 -SlicerDir C:\dev\nocte-builds\run-<newid>
#   powershell -File tools\bbl-compat\make_corpus.ps1 -SlicerDir <dir> -KeepWork
#
# -SlicerDir defaults to the newest C:\dev\nocte-builds\run-* that contains
# orca-slicer.exe. Intermediate files land in tools\bbl-compat\_work (gitignored).
param(
  [string] $SlicerDir,
  [string] $Out,
  [string] $Work,
  [string] $Printer            = "Bambu Lab A1 0.4 nozzle",
  [string] $Process            = "0.20mm Standard @BBL A1",
  [string] $Filament           = "Bambu PLA Basic @BBL A1",
  [string] $BambuStudioVersion = "02.08.02.61",
  [switch] $NoManifest,
  [switch] $KeepWork,
  [switch] $Oracle
)
$ErrorActionPreference = "Stop"

$pyArgs = @(
  "--printer",    $Printer,
  "--process",    $Process,
  "--filament",   $Filament,
  "--bs-version", $BambuStudioVersion
)
if ($SlicerDir)  { $pyArgs += @("--slicer-dir", $SlicerDir) }
if ($Out)        { $pyArgs += @("--out",        $Out) }
if ($Work)       { $pyArgs += @("--work",       $Work) }
if ($NoManifest) { $pyArgs += "--no-manifest" }
if ($KeepWork)   { $pyArgs += "--keep-work" }

python "$PSScriptRoot\make_corpus.py" @pyArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Oracle) {
  $corpus = if ($Out) { $Out } else { "$PSScriptRoot\corpus\nocte-cli" }
  foreach ($case in Get-ChildItem -Directory $corpus) {
    $file = Join-Path $case.FullName "$($case.Name).3mf"
    if (-not (Test-Path $file)) { continue }
    Write-Host "== oracle $($case.Name) =="
    python "$PSScriptRoot\oracle.py" $file `
      --workdir "$PSScriptRoot\_work\oracle\$($case.Name)" `
      --slice --timeout 600 --bs-version $BambuStudioVersion --no-log `
      --json "$PSScriptRoot\_work\oracle\$($case.Name).json"
  }
}
exit 0
