# Fails when the files that repeat the version disagree, or when -Tag (a release tag such as v0.6.0) doesn't name
# that version. The release workflow runs it first, so a tag pushed on the wrong commit stops before anything is built
# or published.
#   powershell -ExecutionPolicy Bypass -File tools\check-version.ps1 [-Tag v0.6.0]
param([string]$Tag)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$header = Get-Content -LiteralPath (Join-Path $root 'src\Version.h') -Raw
$num = { param($name) if ($header -match "#define $name (\d+)") { $Matches[1] } else { throw "$name not found in src\Version.h" } }
if ($header -notmatch '#define OZ_VERSION_STR "([0-9.]+)"') { throw 'OZ_VERSION_STR not found in src\Version.h' }
$version = $Matches[1]
$parts = "$(& $num OZ_VERSION_MAJOR).$(& $num OZ_VERSION_MINOR).$(& $num OZ_VERSION_PATCH)"
[xml]$manifest = Get-Content -LiteralPath (Join-Path $root 'res\app.manifest') -Raw
$manifestVersion = $manifest.assembly.assemblyIdentity.version
$vcpkgVersion = (Get-Content -LiteralPath (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json).'version-string'
if ($parts -ne $version -or $manifestVersion -ne "$version.0" -or $vcpkgVersion -ne $version) {
    throw "Version mismatch: src\Version.h $version ($parts), app.manifest $manifestVersion, vcpkg.json $vcpkgVersion"
}
if ($Tag -and $Tag -ne "v$version") {
    throw "Tag $Tag doesn't match src\Version.h (v$version). Is the tag on the right commit?"
}
if ($Tag) { Write-Host "Version $version, tag $Tag" } else { Write-Host "Version $version" }
