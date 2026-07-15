# Codex Process Guard

Tiny Windows tray app that safely cleans up MCP helper processes left behind after their owning Codex process exits.

## Run

Open `bin\CodexProcessGuard.exe`. The tray icon stays active while the guard is running; Windows may place it under the hidden-icons arrow. Double-click it to open settings, then click **Save** after changing the interval or startup checkbox.

- Check interval: 1–60 minutes (default: 2)
- `Clean now`: runs the same safe scan immediately; one follow-up scan is queued if a scan is already busy
- `Start with Windows`: enabled by default
- Closing the window hides it to the tray; use tray menu → Exit to stop it

## Safety boundary

The guard never kills `codex.exe`, arbitrary Node/Python processes, project servers, or anything still attached to a live Codex process. It only removes an exact `(PID, creation time)` helper that it previously observed under an exact Codex owner, after that owner has disappeared for two consecutive scans. Immediately before removal it checks the owner's native process identity and exit time again; a query failure is treated as "still in use" and skipped.

Duplicate helpers still attached to a live Codex process are shown as **live-attached** and deliberately preserved. When RAM reaches 85%, the tray app warns instead of guessing which live helper is unused. Automatically killing those can interrupt another active Codex task; fully exit Codex once to let the guard clean that owner's leftovers safely.

Settings and redacted logs are stored in `%LocalAppData%\CodexProcessGuard`. Command lines are never written to logs. Tracking state is versioned so a newer classifier never trusts records created by older builds.

## Cohort details

The read-only table groups managed helpers launched within the same eight-second window. It shows launch time, age, exact Codex owner PID, process count, working-set RAM, processes with TCP sockets, executable-type counts, MCP-family counts, and the safety decision. `Older live cohort - preserved` is diagnostic only; it is never an automatic kill decision.

## Build

Run `powershell -ExecutionPolicy Bypass -File .\build.ps1`. It uses the .NET Framework compiler already included with Windows; no package install is required.
