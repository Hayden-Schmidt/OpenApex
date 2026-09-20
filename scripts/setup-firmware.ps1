$ErrorActionPreference = 'Stop'

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw 'ESP-IDF is not installed or its export script is not loaded. Install ESP-IDF 5.x and run export.ps1 first.'
}

Set-Location (Join-Path $PSScriptRoot '..\firmware')
idf.py set-target esp32c3
idf.py reconfigure
idf.py build
