# NØCTE Slicer — end-to-end smoke test on a machine without a compiler.
# STL -> NØCTE 3mf (CI-built binary) -> Bambu Studio oracle (--slice round trip).
# Usage: powershell -File tools\bbl-compat\smoke_export.ps1 -Binary C:\dev\nocte-builds\run-<id>\orca-slicer.exe
param(
  [Parameter(Mandatory = $true)] [string] $Binary,
  [string] $Stl      = "$PSScriptRoot\corpus\fdmhub\PRUEBA_FDMHUB.stl",
  [string] $WorkDir  = "$env:TEMP\nocte-smoke",
  [string] $Printer  = "Bambu Lab A1 0.4 nozzle",
  [string] $Process  = "0.20mm Standard @BBL A1",
  [string] $Filament = "Bambu PLA Basic @BBL A1",
  [string] $BambuStudioVersion = "02.08.02.61"
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$bbl  = "$repo\resources\profiles\BBL"
if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
New-Item -ItemType Directory -Force $WorkDir | Out-Null

$settings = "$bbl\machine\$Printer.json;$bbl\process\$Process.json"
$fil      = "$bbl\filament\$Filament.json"
Write-Host "== NØCTE export ==" 
$p = Start-Process -FilePath $Binary -ArgumentList @(
      "--debug", "2",
      "--load-settings", "`"$settings`"",
      "--load-filaments", "`"$fil`"",
      "--outputdir", $WorkDir,
      "--export-3mf", "nocte_export.3mf",   # relative to --outputdir (absolute paths fail with -13)
      $Stl) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$WorkDir\export-out.txt" -RedirectStandardError "$WorkDir\export-err.txt"
Write-Host "exit=$($p.ExitCode)"
if ($p.ExitCode -ne 0) { Get-Content "$WorkDir\export-err.txt" -Tail 10; exit 2 }

Write-Host "== inspect =="
python "$PSScriptRoot\inspect3mf.py" "$WorkDir\nocte_export.3mf"

Write-Host "== Bambu Studio oracle (slice + re-export) =="
python "$PSScriptRoot\oracle.py" "$WorkDir\nocte_export.3mf" --workdir "$WorkDir\oracle" `
  --timeout 600 --bs-version $BambuStudioVersion --slice --json "$WorkDir\oracle.json"
exit $LASTEXITCODE
