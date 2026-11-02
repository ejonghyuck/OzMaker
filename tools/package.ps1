# Builds Release and packs OzMaker.exe with its license files into dist\OzMaker-<version>-win-x64.zip.
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
$version = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion  # from res\OzMaker.rc

# The version is repeated in a few files; catch a forgotten bump.
[xml]$manifest = Get-Content -LiteralPath (Join-Path $root 'res\app.manifest') -Raw
$manifestVersion = $manifest.assembly.assemblyIdentity.version
$vcpkgVersion = (Get-Content -LiteralPath (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json).'version-string'
$footer = Select-String -LiteralPath (Join-Path $root 'src\AppViews.cpp') -Pattern 'L"OzMaker (\d+\.\d+)' | Select-Object -First 1
$footerVersion = if ($footer) { $footer.Matches[0].Groups[1].Value } else { '?' }
if ($manifestVersion -ne "$version.0" -or $vcpkgVersion -ne $version -or -not $version.StartsWith("$footerVersion.")) {
    Write-Warning "Version mismatch: OzMaker.rc $version, app.manifest $manifestVersion, vcpkg.json $vcpkgVersion, settings footer $footerVersion"
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
Remove-Item -LiteralPath $stage -Recurse -Force

$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
Write-Host ''
Write-Host "Created $zip"
Write-Host ('{0:N0} bytes, SHA-256 {1}' -f (Get-Item -LiteralPath $zip).Length, $hash)
