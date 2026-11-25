param(
    [int]$Seconds = 120,
    [string[]]$Targets,
    [int]$Parallel = 4,
    [switch]$AsanOnly,
    [switch]$NoBuild
)

# Builds the tests with libFuzzer and AddressSanitizer (Visual Studio's clang)
# and fuzzes the parsers; see Docs/DEVELOPMENT.md, "Fuzzing and AddressSanitizer".
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
$clang = Get-ChildItem (Join-Path $vs 'VC\Tools\Llvm\x64\lib\clang') -Directory | Sort-Object { [int]$_.Name } | Select-Object -Last 1
$runtime = Join-Path $clang.FullName 'lib\windows'
$release = Join-Path $root 'Build\x64\Release'
$out = Join-Path $root ($(if ($AsanOnly) { 'Build\asan' } else { 'Build\fuzzbin' }))
$obj = Join-Path $root ($(if ($AsanOnly) { 'Build\objasan\Tests' } else { 'Build\objfuzz\Tests' }))
$all = 'table', 'xml', 'deflate', 'tablefile', 'image', 'ceform', 'structs', 'reclass', 'theme', 'ini', 'asm', 'autoassembler', 'signature', 'managed', 'clr', 'autorun', 'pointerscan'

if (-not $NoBuild)
{
    if (-not (Test-Path (Join-Path $release 'Dependencies.lib'))) { throw "Build Release|x64 first: the sanitized tests link its Dependencies.lib and use its test targets." }
    New-Item -ItemType Directory -Force $out | Out-Null
    robocopy $release $out /E /XF Manifold-Engine.Tests.* /NFL /NDL /NJH /NJS /NP | Out-Null
    Copy-Item (Join-Path $runtime 'clang_rt.asan_dynamic-x86_64.dll') $out -Force
    $sanitize = if ($AsanOnly) { '-fsanitize=address' } else { "-fsanitize=fuzzer-no-link,address -fsanitize-coverage-ignorelist=$(Join-Path $root 'Tools\fuzz-coverage-ignore.txt') /DMANIFOLD_FUZZ" }
    $env:_CL_ = "$sanitize -fsanitize-address-use-after-return=never -mllvm -asan-stack=0 /Oy-"
    $libs = '/wholearchive:clang_rt.asan-x86_64.lib /wholearchive:clang_rt.asan_cxx-x86_64.lib'
    if (-not $AsanOnly) { $libs += ' clang_rt.fuzzer_no_main-x86_64.lib' }
    $env:_LINK_ = "/LIBPATH:`"$runtime`" $libs"
    try
    {
        & $msbuild (Join-Path $root 'Projects\Manifold-Engine.Tests.vcxproj') /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:BuildProjectReferences=false "/p:OutDir=$out\" "/p:IntDir=$obj\" /nodeReuse:false /m /v:minimal /nologo
        if ($LASTEXITCODE -ne 0) { throw "The sanitized build failed." }
    }
    finally
    {
        Remove-Item Env:_CL_, Env:_LINK_ -ErrorAction SilentlyContinue
    }
}
if ($AsanOnly)
{
    Write-Output "Built $out\Manifold-Engine.Tests.exe. Run it with ASAN_OPTIONS=detect_odr_violation=0:halt_on_error=1."
    return
}

$env:ASAN_OPTIONS = 'detect_odr_violation=0:detect_container_overflow=0'
$fuzz = Join-Path $root 'Build\fuzz'
$found = Join-Path $fuzz 'found'
New-Item -ItemType Directory -Force $found, (Join-Path $fuzz 'logs') | Out-Null
$wanted = if ($Targets) { $Targets } else { $all }
$jobs = @()
foreach ($target in $wanted)
{
    if ($all -notcontains $target) { throw "No fuzz target is called $target. Targets: $($all -join ', ')" }
    $work = Join-Path $fuzz "work\$target"
    $seeds = Join-Path $root "Tests\Fuzz\$target"
    New-Item -ItemType Directory -Force $work, $seeds | Out-Null
    $length = if ('table', 'xml', 'clr', 'reclass', 'ceform', 'managed', 'pointerscan', 'image', 'deflate' -contains $target) { 65536 } else { 4096 }
    while (@($jobs | Where-Object { -not $_.Process.HasExited }).Count -ge $Parallel) { Start-Sleep -Milliseconds 500 }
    $arguments = @('--fuzz', $target, "`"$work`"", "`"$seeds`"", "-max_total_time=$Seconds", "-max_len=$length", '-timeout=10', '-rss_limit_mb=3072', '-malloc_limit_mb=1024', "-artifact_prefix=`"$found\$target-`"")
    $process = Start-Process (Join-Path $out 'Manifold-Engine.Tests.exe') -ArgumentList $arguments -WorkingDirectory $out -NoNewWindow -PassThru -RedirectStandardError (Join-Path $fuzz "logs\$target.log") -RedirectStandardOutput (Join-Path $fuzz "logs\$target.out")
    $null = $process.Handle
    $process.PriorityClass = 'BelowNormal'
    $jobs += [pscustomobject]@{ Target = $target; Process = $process }
}
foreach ($job in $jobs)
{
    $job.Process.WaitForExit()
    $line = Select-String -Path (Join-Path $fuzz "logs\$($job.Target).log") -Pattern 'DONE|ERROR|deadly|out-of-memory|timeout' | Select-Object -First 1
    Write-Output ("{0,-14} exit {1}  {2}" -f $job.Target, $job.Process.ExitCode, $(if ($line) { $line.Line.Trim() } else { '' }))
}
$broken = @(Get-ChildItem $found -File -ErrorAction SilentlyContinue)
Write-Output "$($broken.Count) inputs in $found broke a parser."
