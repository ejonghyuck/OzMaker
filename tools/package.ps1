# Builds Release and packs OzMaker.exe with its license files into dist\OzMaker-<version>-win-x64.zip, plus
# dist\OzMaker.exe and dist\update.txt for the updater. The folder the zip holds stays unpacked next to it (CI uploads
# it as the build artifact).
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-NoBuild]
param([switch]$NoBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $NoBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found. Install Visual Studio 2022 with the C++ desktop workload.' }
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild not found.' }
    & $msbuild (Join-Path $root 'OzMaker.vcxproj') /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit code $LASTEXITCODE)." }
}

$exe = Join-Path $root 'bin\Release\OzMaker.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Not built yet: $exe" }
$version = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion  # src\Version.h, through res\OzMaker.rc

# The version is repeated in a few files; catch a forgotten bump.
[xml]$manifest = Get-Content -LiteralPath (Join-Path $root 'res\app.manifest') -Raw
$manifestVersion = $manifest.assembly.assemblyIdentity.version
$vcpkgVersion = (Get-Content -LiteralPath (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json).'version-string'
if ($manifestVersion -ne "$version.0" -or $vcpkgVersion -ne $version) {
    Write-Warning "Version mismatch: src\Version.h $version, app.manifest $manifestVersion, vcpkg.json $vcpkgVersion"
}

$name = "OzMaker-$version-win-x64"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
$zip = Join-Path $dist "$name.zip"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item -LiteralPath $exe -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $stage 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'THIRD-PARTY-NOTICES.md') -Destination $stage

# A vcpkg build links WebP/AVIF libraries statically; their licenses must travel with the exe.
$installed = Join-Path $root 'vcpkg_installed'
if (Test-Path -LiteralPath $installed) {
    $licenses = Get-ChildItem -LiteralPath $installed -Recurse -Filter copyright -File |
        Where-Object { $_.FullName -like '*\x64-windows-static-rel\share\*' -and $_.Directory.Name -notlike 'vcpkg-*' }
    if ($licenses) {
        $dir = Join-Path $stage 'licenses'
        New-Item -ItemType Directory -Path $dir | Out-Null
        foreach ($f in $licenses) { Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $dir ($f.Directory.Name + '.txt')) }
        Write-Host ('Library licenses: ' + (($licenses | ForEach-Object { $_.Directory.Name }) -join ', '))
    }
}

Compress-Archive -Path $stage -DestinationPath $zip

# What the updater in earlier versions reads (src\Updater.h): the exe on its own, and update.txt describing it.
$exeOut = Join-Path $dist 'OzMaker.exe'
Copy-Item -LiteralPath $exe -Destination $exeOut -Force
$exeHash = (Get-FileHash -LiteralPath $exeOut -Algorithm SHA256).Hash.ToLowerInvariant()
$info = "version=$version`r`nexe=OzMaker.exe`r`nsize=$((Get-Item -LiteralPath $exeOut).Length)`r`nsha256=$exeHash`r`n"
[IO.File]::WriteAllText((Join-Path $dist 'update.txt'), $info, [Text.Encoding]::ASCII)

$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
Write-Host ''
Write-Host "Created $zip, OzMaker.exe and update.txt"
Write-Host ('{0:N0} bytes, SHA-256 {1}' -f (Get-Item -LiteralPath $zip).Length, $hash)
