# Codex Process Guard Native

A dependency-free C++20/Win32 rewrite of Codex Process Guard for Windows. Automatic cleanup preserves every recognized helper whose exact Codex owner is alive. There is no age or idle timeout; cleanup requires that exact owner identity to be missing for the same helper in two consecutive observing scans.

## Safety model

- Process identity is `(PID, creation time)`, preventing PID-reuse mistakes.
- Parent lineage must resolve to an exact live `codex.exe` identity.
- Only known helper executables with recognized Codex/MCP command lines are managed.
- Live Codex owners are never automatic termination candidates.
- Manual removal is limited to an older cohort and repeats a fresh identity, lineage, classifier, and cohort check.
- Unreadable process metadata fails closed: the process is preserved.

## Evidence shown in the UI

The summary panel shows confirmed cleanup over the last 60 minutes, observed pre-kill process working set (not measured system RAM reduction), currently protected/suspicious helpers, and the Guard's own cost. Confirmed automatic and manual actions are written to the visible log with helper PID, creation time, owner PID, and observed pre-kill working set when available. The UI summary is bounded to 60 minutes; cleanup history is pruned and rewritten atomically whenever a confirmed cleanup is recorded. Malformed or unwritable history is disclosed in the UI.

## Build and test

Requirements: Visual Studio 2022 Build Tools with Desktop C++, Windows SDK, and CMake.

```powershell
.\build.ps1
```

The release executable is written to `bin\CodexProcessGuardNative.exe`. To run the test suite directly:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

This repository intentionally uses no package manager, GUI framework, test framework, service, or installer. The tests are a small native executable covering entrypoint classification and false positives, PID reuse, two-scan orphan policy, exact cohort matching, process inspection, TCP ownership, confirmed exact termination, the tracking state machine, the destructive coordinator's fail-closed behavior, and evidence aggregation.

## Use

Run the executable and leave it in the notification area. The default interval is two minutes. Change the interval or startup option and select **Save** to apply it. Closing the window hides it to the tray; right-click the tray icon and select **Exit** to stop it completely. Startup with Windows is off by default so this trial build cannot silently compete with another installed guard.

Windows Smart App Control can block any locally built unsigned binary. Do not disable Smart App Control. For distribution, sign the executable with a trusted code-signing certificate.

Runtime data is stored under `%LOCALAPPDATA%\CodexProcessGuardNative`: `guard.log`, `tracked.tsv`, `settings.ini`, `evidence.ini`, and `cleanup.tsv`.
