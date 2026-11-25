<#
    Packs a Release build into the zip files of a GitHub release:
      Manifold-Engine-<Version>-win-x64.zip          the program, its helpers, autorun modules and licenses
      Manifold-Engine-<Version>-win-x64-symbols.zip  the .pdb files, for reading crash dumps
    Build Release|x64 first. The release workflow (.github/workflows/release.yml) runs this on a tag;
    it also works locally:  .\Tools\Package-Release.ps1 -Version 1.0.0
#>
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$Destination = 'dist'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'Build\x64\Release'
if (-not [System.IO.Path]::IsPathRooted($Destination)) { $Destination = Join-Path $root $Destination }
$name = "Manifold-Engine-$Version-win-x64"
$stage = Join-Path $Destination $name
$symbols = Join-Path $Destination "$name-symbols"

# The same program files as Tools\Install.ps1, without the folders a user's install keeps (configs, tables, logs ...).
$programs = 'Manifold-Engine.exe', 'ManifoldAgent.dll', 'ManifoldAgent32.dll', 'ManifoldClr.exe', 'ManifoldClr32.exe'
foreach ($file in $programs)
{
    if (-not (Test-Path (Join-Path $build $file))) { throw "$build\$file does not exist. Build Release|x64 first." }
}

foreach ($folder in $stage, $symbols)
{
    if (Test-Path $folder) { Remove-Item -Recurse -Force $folder }
    New-Item -ItemType Directory -Force $folder | Out-Null
}

foreach ($file in $programs)
{
    Copy-Item (Join-Path $build $file) $stage
    $pdb = [System.IO.Path]::ChangeExtension($file, '.pdb')
    if (Test-Path (Join-Path $build $pdb)) { Copy-Item (Join-Path $build $pdb) $symbols }
}

# autorun\modules as the repository has it; the program creates its other folders on first start
Copy-Item -Recurse (Join-Path $root 'Autorun') (Join-Path $stage 'autorun')
# Help > About says the licenses are in the Licenses folder next to the program
Copy-Item -Recurse (Join-Path $root 'Licenses') (Join-Path $stage 'Licenses')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')

$zip = Join-Path $Destination "$name.zip"
$symbolsZip = Join-Path $Destination "$name-symbols.zip"
foreach ($file in $zip, $symbolsZip)
{
    if (Test-Path $file) { Remove-Item -Force $file }
}
# the zip holds one folder named like the zip, so unpacking it never spills files into the folder it lands in
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Compress-Archive -Path (Join-Path $symbols '*') -DestinationPath $symbolsZip -CompressionLevel Optimal
Remove-Item -Recurse -Force $stage, $symbols

foreach ($file in $zip, $symbolsZip)
{
    $hash = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant()
    Write-Output ("{0}  {1}  ({2:N1} MB)" -f $hash, (Split-Path -Leaf $file), ((Get-Item $file).Length / 1MB))
}
