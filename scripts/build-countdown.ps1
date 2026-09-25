$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
New-Item -ItemType Directory -Force -Path build\host | Out-Null

$common = @('firmware\main\countdown.c', 'firmware\main\packet.c', 'firmware\main\normalize.c', 'firmware\main\pipeline.c', 'firmware\main\heading_fusion.c')

# A GCC installed under a path containing a space is unusable: its `ld` splits the internal
# -L path on the space and fails looking for default-manifest.o under the truncated prefix.
# Short 8.3 names do not help (ld re-expands them). Treat such a toolchain as absent and fall
# through to MSVC rather than failing the whole script -- WinLibs under C:\Users\<first last>\
# is the common case on this project's dev machines.
function Find-Compiler($name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c -and $c.Source -notmatch ' ') { return $c }
    return $null
}

$compiler = Find-Compiler cc
if (-not $compiler) { $compiler = Find-Compiler gcc }
if (-not $compiler) { $compiler = Find-Compiler clang }

$defs = '/D_CRT_SECURE_NO_WARNINGS'

if ($compiler) {
    # GCC/Clang-style
    & $compiler.Source -std=c11 -Wall -Wextra -Werror @common firmware\test_host\countdown_test.c -o build\host\countdown_test.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $compiler.Source -std=c11 -Wall -Wextra -Werror @common firmware\test_host\packet_normalize_test.c -o build\host\packet_normalize_test.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $compiler.Source -std=c11 -Wall -Wextra -Werror @common firmware\test_host\pipeline_test.c -o build\host\pipeline_test.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $compiler.Source -std=c11 -Wall -Wextra -Werror @common firmware\test_host\heading_fusion_test.c -o build\host\heading_fusion_test.exe
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
    # cl scatters .obj into the current directory and refuses /Fo with multiple sources, so build
    # from a scratch dir -- otherwise the repo root fills with countdown.obj, vc140.pdb and friends.
    New-Item -ItemType Directory -Force -Path build\obj | Out-Null
    $srcs = (($common | ForEach-Object { "..\..\$_" }) -join ' ')
    foreach ($t in @('countdown_test', 'packet_normalize_test', 'pipeline_test', 'heading_fusion_test')) {
        cmd /c "`"$vcvars`" >nul 2>&1 && cd /d build\obj && cl /nologo /std:c11 /TC /W3 $defs $srcs ..\..\firmware\test_host\$t.c /Fe:..\..\build\host\$t.exe"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}

& build\host\countdown_test.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& build\host\packet_normalize_test.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& build\host\pipeline_test.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& build\host\heading_fusion_test.exe
