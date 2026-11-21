param(
    [string]$Destination,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$AdoptFrom
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Get-MainCheckout
{
    $common = git -C $root rev-parse --git-common-dir 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $common) { return $root }
    if (-not [System.IO.Path]::IsPathRooted($common)) { $common = Join-Path $root $common }
    return Split-Path -Parent ([System.IO.Path]::GetFullPath($common))
}

function Copy-Missing([string]$From, [string]$To)
{
    if (-not (Test-Path $From)) { return }
    robocopy $From $To /E /XC /XN /XO /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying $From to $To failed (robocopy $LASTEXITCODE)" }
}

if (-not $Destination) { $Destination = Join-Path (Get-MainCheckout) 'Install' }
$Destination = [System.IO.Path]::GetFullPath($Destination)
$build = Join-Path $root "Build\x64\$Configuration"
$program = Join-Path $build 'Manifold-Engine.exe'
if (-not (Test-Path $program)) { throw "$program does not exist. Build $Configuration|x64 first." }

$installed = Join-Path $Destination 'Manifold-Engine.exe'
$running = Get-Process -Name 'Manifold-Engine' -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ([System.IO.Path]::GetFullPath($_.Path) -eq $installed) }
if ($running) { throw "Manifold Engine is running from $Destination (pid $($running.Id -join ', ')). Close it, then install again." }

New-Item -ItemType Directory -Force $Destination | Out-Null
$first = -not (Test-Path (Join-Path $Destination 'configs'))
if ($first)
{
    if (-not $AdoptFrom) { $AdoptFrom = Join-Path (Get-MainCheckout) 'Build\x64\Release' }
    if ((Test-Path (Join-Path $AdoptFrom 'configs')) -and ([System.IO.Path]::GetFullPath($AdoptFrom) -ne $Destination))
    {
        foreach ($folder in 'configs', 'tables', 'sounds', 'templates', 'autorun')
        {
            Copy-Missing (Join-Path $AdoptFrom $folder) (Join-Path $Destination $folder)
        }
        Write-Output "Copied configs, tables, sounds, templates and autorun from $AdoptFrom (the originals stay where they are)."
    }
}

foreach ($file in 'Manifold-Engine.exe', 'Manifold-Engine.pdb', 'ManifoldAgent.dll', 'ManifoldAgent.pdb', 'ManifoldAgent32.dll', 'ManifoldAgent32.pdb', 'ManifoldClr.exe', 'ManifoldClr.pdb', 'ManifoldClr32.exe', 'ManifoldClr32.pdb')
{
    $from = Join-Path $build $file
    if (Test-Path $from) { Copy-Item $from (Join-Path $Destination $file) -Force }
}

$autorun = Join-Path $root 'Autorun'
if (Test-Path $autorun)
{
    robocopy $autorun (Join-Path $Destination 'autorun') /E /XO /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying the Autorun modules failed (robocopy $LASTEXITCODE)" }
}

$commit = git -C $root rev-parse --short=7 HEAD 2>$null
Write-Output "Installed $Configuration build $commit into $Destination."
Write-Output "Start it with: & '$installed'"
