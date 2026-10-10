# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Repository governance: `LICENSE` (MIT), `CONTRIBUTING.md`, issue/PR templates,
  firmware version string (`src/version.h`) printed in the boot log.

### Changed

- Docs: correct the `src/ui/` / `usage_display.h` project-structure entries and
  the test-coverage notes in `README.md` / `AGENTS.md`.

### Removed

- `compile_commands.json` is no longer tracked (build artifact).

## [0.1.0] - 2026-10-09

Initial tracked baseline: three-page UI (Weather / Usage / Clock) on the Waveshare
ESP32-S3-Touch-AMOLED-2.16, OAuth device flow, WiFi setup UI, dirty-region display
pushes, and host unit tests for the pure helpers and parsers.

[Unreleased]: https://github.com/ahsar/esp32-usage-monitor/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/ahsar/esp32-usage-monitor/releases/tag/v0.1.0
