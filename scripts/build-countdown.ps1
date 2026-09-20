$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
New-Item -ItemType Directory -Force -Path build\host | Out-Null

$common = @('firmware\main\countdown.c', 'firmware\main\packet.c', 'firmware\main\normalize.c', 'firmware\main\pipeline.c')

$compiler = Get-Command cc -ErrorAction SilentlyContinue
if (-not $compiler) { $compiler = Get-Command gcc -ErrorAction SilentlyContinue }
if (-not $compiler) { $compiler = Get-Command clang -ErrorAction SilentlyContinue }

$defs = '/D_CRT_SECURE_NO_WARNINGS'

if ($compiler) {
    # GCC/Clang-style
    & $compiler.Source -std=c11 -Wall -Wextra -Werror @common firmware\test_host\countdown_test.c -o build\host\countdown_test.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $compiler.Source -std=c11 -Wall -Wextra -Werror @common firmware\test_host\packet_normalize_test.c -o build\host\packet_normalize_test.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $compiler.Source -std=c11 -Wall -Wextra -Werror @common firmware\test_host\pipeline_test.c -o build\host\pipeline_test.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    # MSVC fallback (no gcc/clang on PATH). Requires VS Build Tools C++ workload.
    $vcvars = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        $vcvars = Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    }
    if (-not (Test-Path $vcvars)) {
        throw 'Install a C compiler (LLVM/Clang, MinGW, or Visual Studio Build Tools) before running the host tests.'
    }
    $srcs = ($common -join ' ')
    cmd /c "`"$vcvars`" >nul 2>&1 && cl /nologo /std:c11 /TC /W3 $defs $srcs firmware\test_host\countdown_test.c /Fe:build\host\countdown_test.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    cmd /c "`"$vcvars`" >nul 2>&1 && cl /nologo /std:c11 /TC /W3 $defs $srcs firmware\test_host\packet_normalize_test.c /Fe:build\host\packet_normalize_test.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    cmd /c "`"$vcvars`" >nul 2>&1 && cl /nologo /std:c11 /TC /W3 $defs $srcs firmware\test_host\pipeline_test.c /Fe:build\host\pipeline_test.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& build\host\countdown_test.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& build\host\packet_normalize_test.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& build\host\pipeline_test.exe
