# Codex Process Guard

Tiny Windows tray app that safely cleans up MCP helper processes left behind after their owning Codex process exits.

## Run

Open `bin\CodexProcessGuard.exe`. The tray icon stays active while the guard is running; Windows may place it under the hidden-icons arrow. Double-click it to open settings, then click **Save** after changing the interval or startup checkbox.

- Check interval: 1–60 minutes (default: 2)
- `Scan now`: runs the same automatic safe scan immediately; one follow-up scan is queued if a scan is already busy
- `Kill selected older`: manually removes only the selected **Older live cohort** after a warning and a fresh exact-identity revalidation
- `Start with Windows`: enabled by default
- Closing the window hides it to the tray; use tray menu → Exit to stop it

## Safety boundary

Automatic cleanup never kills `codex.exe`, arbitrary Node/Python processes, project servers, or anything still attached to a live Codex process. It only removes an exact `(PID, creation time)` helper that it previously observed under an exact Codex owner, after that owner has disappeared for two consecutive scans. Immediately before removal it checks the owner's native process identity and exit time again; a query failure is treated as "still in use" and skipped.

Duplicate helpers still attached to a live Codex process are shown as **live-attached** and deliberately preserved. When RAM reaches 85%, the tray app warns instead of guessing which live helper is unused. Automatically killing those can interrupt another active Codex task; fully exit Codex once to let the guard clean that owner's leftovers safely.

Manual removal is intentionally narrower than a generic End task button. It is enabled only for a selected row marked **Older live cohort** and always shows a confirmation warning. After confirmation, Guard takes a new process snapshot and requires the exact Codex owner plus the complete set of helper `(PID, creation time)` identities to still match. It also rechecks every helper's classifier and ancestry before terminating helper processes. It never terminates `codex.exe`. If anything changed, the request is refused without guessing.

Settings and redacted logs are stored in `%LocalAppData%\CodexProcessGuard`. Command lines are never written to logs. Tracking state is versioned so a newer classifier never trusts records created by older builds.

## Cohort details

The table groups managed helpers launched within the same eight-second window. It shows launch time, age, exact Codex owner PID, process count, working-set RAM, processes with TCP sockets, executable-type counts, MCP-family counts, and the safety decision. `Older live cohort - preserved` is never an automatic kill decision; it only makes that exact row eligible for the explicit manual action above.

## Build

Run `powershell -ExecutionPolicy Bypass -File .\build.ps1`. It uses the .NET Framework compiler already included with Windows; no package install is required.

Windows 11 Smart App Control may block a newly compiled unsigned executable. Do not turn off machine-wide protection just for this app. Use an RSA code-signing certificate from a trusted provider, or build/test it in a development environment where that policy is not enforcing. This repository does not publish a signed binary. See Microsoft's [Smart App Control signing guidance](https://learn.microsoft.com/windows/apps/develop/smart-app-control/code-signing-for-smart-app-control).
