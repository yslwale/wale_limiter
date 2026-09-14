# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Every release bundles whatever ReShade was latest at build time; the exact version is recorded in
`reshade-version.txt` inside the zip.

## [Unreleased]

## [1.0.0] - 2026-09-14

First release, built for the **Underground Racing** server.

### Added
- Hard 60 FPS frame cap as a ReShade add-on (add-on API 11, so ReShade 6.1 and newer), switched
  on and off with one checkbox.
- The on/off choice is saved in `ReShade.ini` (`[wale_limiter] LimitTo60`).
- English / Hungarian language switch: 🇬🇧 / 🇭🇺 flag buttons in the add-on's window. The choice
  is saved (`Language=en|hu`); on first run the Windows display language decides.
- Underground Racing logo, a Discord button (<https://discord.gg/undergroundracing>) and a
  "View Source on GitHub" button.
- Release zip bundling the latest official ReShade, `Enable-ReShade.bat` and an install guide.
- `build.ps1`: build the add-on (and optionally the release zip) without Visual Studio or CMake.
