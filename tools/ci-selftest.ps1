# Runs OzMaker.exe --selftest with a throwaway settings file and fails if any check failed.
#   -Require WebP,AVIF   also fail when those encoders were not built in.
param([string[]]$Require = @())

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path ([IO.Path]::GetTempPath()) ('ozmaker-selftest-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$env:OZMAKER_SETTINGS = Join-Path $work 'settings.ini'

$exe = Join-Path $root 'bin\Release\OzMaker.exe'
$p = Start-Process -FilePath $exe -ArgumentList ('--selftest "' + $work + '"') -PassThru
if (-not $p.WaitForExit(300000)) {
    $p.Kill()
    throw 'Self-test did not finish within 5 minutes.'
}

$log = Join-Path $work 'selftest.log'
if (-not (Test-Path -LiteralPath $log)) { throw "No self-test log (exit code $($p.ExitCode))." }
$lines = Get-Content -LiteralPath $log -Encoding UTF8
$lines | ForEach-Object { Write-Host $_ }

$failed = @()
if (-not ($lines -match '^RESULT: all passed')) { $failed += 'self-test' }
foreach ($f in $Require) {
    if (-not ($lines -match ('^' + [regex]::Escape($f) + ': ok'))) { $failed += $f }
}
if ($failed) { throw ('Failed: ' + ($failed -join ', ')) }
