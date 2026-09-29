$ErrorActionPreference = 'Stop'

if (-not $env:ANDROID_HOME -and -not $env:ANDROID_SDK_ROOT) {
    Write-Warning 'ANDROID_HOME/ANDROID_SDK_ROOT is not set. Install Android Studio or the Android command-line tools first.'
}

Set-Location (Join-Path $PSScriptRoot '..')
if (-not (Test-Path .\phone\gradlew.bat)) {
    throw 'Gradle wrapper is missing. Run from a repository containing the wrapper files.'
}
& .\phone\gradlew.bat --version
& .\phone\gradlew.bat -p phone dependencies
