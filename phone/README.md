# OpenApex Android Auto module

This is the Android Auto host-facing shell for OpenApex Phase 1. It is intentionally minimal:
notification ingestion, phone GNSS normalization, and BLE publishing will be added behind this
module without bringing local maps or routing into the project.

Build with `..\gradlew.bat -p android assembleDebug` from the repository root after Android SDK setup. The module
uses the Android for Cars App Library and exposes a navigation-category `CarAppService`.
