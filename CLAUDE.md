# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**ClaudeTray** — A Windows system tray utility (Qt6/C++17) that monitors Claude Code API usage quotas in real-time, using OAuth-based API polling with local JSONL file scanning as a fallback.

## Build Commands

```bash
# Configure (from project root)
cmake -B build -S .

# Build (Debug)
cmake --build build --config Debug

# Build (Release)
cmake --build build --config Release
```

Also opens directly in Qt Creator via `CMakeLists.txt`. No test framework is present.

**Requirements:** Qt6 (Widgets, Network, Concurrent), CMake 3.5+, C++17 compiler, Windows 10/11. MinGW 64-bit recommended.

## Architecture

### Data Sources (Priority Order)

1. **Primary — API:** `GET https://api.anthropic.com/api/oauth/usage`
   - Bearer token from `~/.claude/.credentials.json`
   - Returns `utilization` (0.0–1.0) + `resets_at` per quota period
   - Polled every 5 minutes; retries on failure (30s delays, up to 3x)

2. **Fallback — Local JSONL:** `~/.claude/projects/**/*.jsonl`
   - Token counts parsed per record, deduplicated by `uuid`
   - Hardcoded plan limits: Pro 18M/144M, Max5x 90M/720M, Max20x 360M/2880M (5h/7d)
   - File-watched in real-time (300ms debounce)

3. **Hybrid merge:** After an API success followed by failure, local deltas are added to the last API baseline.

### Class Responsibilities

| Class | Role |
|---|---|
| `TrayApp` | Central orchestrator — connects signals, merges data sources, drives UI updates |
| `UsageApiClient` | QNetworkAccessManager wrapper; emits `usageFetched` / `fetchFailed` |
| `UsageScanner` | QFileSystemWatcher on JSONL files; emits `localUsageUpdated`, `activityDetected` |
| `UsagePopup` | Frameless draggable popup: two quota panels, pin button, refresh status, opacity fade |
| `QuotaPanel` / `ThresholdBar` | Custom widgets — 12px progress bar with shimmer, color-coded by threshold |
| `CredentialsReader` | Static helpers to read OAuth token and subscription type from credentials file |
| `UsageData` / `QuotaInfo` | Shared data structures; warning constants (USAGE_WARN_PCT=71%, USAGE_CRIT_PCT=86%) |

### Signal Flow

```
UsageApiClient ──usageFetched()──► TrayApp ──► UsagePopup (UI update)
               ──fetchFailed()──►            ──► Tray icon / tooltip

UsageScanner ──localUsageUpdated()──► TrayApp (fallback or delta merge)
             ──activityDetected()──►  TrayApp ──► shimmer animation
```

### Key Behaviors

- **Non-blocking I/O:** `QtConcurrent::run()` for JSONL scanning; `QNetworkAccessManager` for HTTP.
- **Drag suppression:** Pending UI updates are queued during window drag and applied on release.
- **Opacity fade:** 10s idle → 0.6 opacity animation (only when pinned).
- **Reset detection:** Tracks `resets_at`; auto-fetches fresh data when a reset time passes.
- **QSettings persistence:** `HKEY_CURRENT_USER\Software\ClaudeTray\ClaudeTray` stores `reset5h`, `reset7d` as ISO datetimes.

## UI Notes

- Popup is 280px wide, frameless, draggable by title bar.
- ThresholdBar colors: Green (0–70%) → Orange (71–85%) → Red (86–100%).
- Shimmer effect activates on JSONL writes (active Claude usage), stops after 2s idle.
- Countdown text format: `"86m 후 초기화 (18:00)"` / `"1h 26m 후"` / `"2d 3h 후 (4/17 목)"`.
- UI text is in Korean.
