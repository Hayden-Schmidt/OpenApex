$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
New-Item -ItemType Directory -Force -Path build\host | Out-Null
$compiler = Get-Command cc -ErrorAction SilentlyContinue
if (-not $compiler) { $compiler = Get-Command gcc -ErrorAction SilentlyContinue }
if (-not $compiler) { throw 'Install a C compiler (for example LLVM/Clang or MinGW) before running the host countdown test.' }
& $compiler.Source -std=c11 -Wall -Wextra -Werror firmware\main\countdown.c firmware\test_host\countdown_test.c -o build\host\countdown_test.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& build\host\countdown_test.exe
