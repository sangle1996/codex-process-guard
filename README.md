# Codex Process Guard Native

A dependency-free C++20/Win32 rewrite of Codex Process Guard for Windows. It watches only recognized Codex helper processes, keeps every helper whose exact Codex owner is alive, and requires the same orphan to be observed in two scans before automatic termination.

## Safety model

- Process identity is `(PID, creation time)`, preventing PID-reuse mistakes.
- Parent lineage must resolve to an exact live `codex.exe` identity.
- Only known helper executables with recognized Codex/MCP command lines are managed.
- Live Codex owners are never automatic termination candidates.
- Manual removal is limited to an older cohort and repeats a fresh identity, lineage, classifier, and cohort check.
- Unreadable process metadata fails closed: the process is preserved.

## Build and test

Requirements: Visual Studio 2022 Build Tools with Desktop C++, Windows SDK, and CMake.

```powershell
.\build.ps1
```

The release executable is written to `bin\CodexProcessGuardNative.exe`. To run the test suite directly:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

This repository intentionally uses no package manager, GUI framework, test framework, service, or installer. The tests are a small native executable covering entrypoint classification and false positives, PID reuse, two-scan orphan policy, exact cohort matching, process inspection, TCP ownership, exact termination, the tracking state machine, and the destructive coordinator's fail-closed behavior.

## Use

Run the executable and leave it in the notification area. The default interval is two minutes. Change the interval or startup option and select **Save** to apply it. Closing the window hides it to the tray; right-click the tray icon and select **Exit** to stop it completely. Startup with Windows is off by default so this trial build cannot silently compete with another installed guard.

Windows Smart App Control can block any locally built unsigned binary. Do not disable Smart App Control. For distribution, sign the executable with a trusted code-signing certificate.

Runtime data is stored under `%LOCALAPPDATA%\CodexProcessGuardNative`: `guard.log`, `tracked.tsv`, and `settings.ini`.
