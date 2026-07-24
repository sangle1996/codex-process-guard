param([switch]$Background)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$mutex = New-Object Threading.Mutex($true, 'Local\CodexProcessGuardPs')
if (-not $mutex.WaitOne(0, $false)) { exit 0 }

$dataDir = Join-Path $env:LOCALAPPDATA 'CodexProcessGuardPs'
$null = New-Item -ItemType Directory -Force -Path $dataDir
$logPath = Join-Path $dataDir 'guard.log'
$tracked = @{}
$killedTotal = 0

function Write-GuardLog($Text) {
  $line = "[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Text
  Add-Content -Path $logPath -Value $line
  if ($script:logBox) {
    $script:logBox.AppendText($line + [Environment]::NewLine)
  }
}

function Is-Helper($p) {
  $name = ($p.Name + '').ToLowerInvariant()
  if ($name -notin @('node.exe','node_repl.exe','python.exe','pythonw.exe','uv.exe','uvx.exe')) { return $false }
  $cmd = ($p.CommandLine + '').ToLowerInvariant()
  return $name -eq 'node_repl.exe' -or $cmd.Contains('.codex') -or $cmd.Contains('mcp') -or $cmd.Contains('modelcontextprotocol')
}

function Find-CodexOwner($p, $byPid) {
  $seen = @{}
  $cur = $p
  for ($i = 0; $i -lt 64 -and $cur; $i++) {
    if ($seen.ContainsKey([int]$cur.ProcessId)) { return $null }
    $seen[[int]$cur.ProcessId] = $true
    if (($cur.Name + '').ToLowerInvariant() -eq 'codex.exe') { return $cur }
    $cur = $byPid[[int]$cur.ParentProcessId]
  }
  return $null
}

function Scan-Guard {
  $procs = Get-CimInstance Win32_Process
  $byPid = @{}
  foreach ($p in $procs) { $byPid[[int]$p.ProcessId] = $p }
  $codexLive = @{}
  foreach ($p in $procs) {
    if (($p.Name + '').ToLowerInvariant() -eq 'codex.exe') {
      $codexLive["$($p.ProcessId):$($p.CreationDate)"] = $true
    }
  }

  $liveAttached = 0
  $pending = 0
  $killed = 0
  foreach ($p in $procs) {
    if (-not (Is-Helper $p)) { continue }
    $owner = Find-CodexOwner $p $byPid
    if ($owner) {
      $key = "$($p.ProcessId):$($p.CreationDate)"
      $tracked[$key] = [pscustomobject]@{
        Pid = [int]$p.ProcessId
        Created = $p.CreationDate
        OwnerKey = "$($owner.ProcessId):$($owner.CreationDate)"
        SeenOrphan = 0
      }
      $liveAttached++
    }
  }

  foreach ($key in @($tracked.Keys)) {
    $t = $tracked[$key]
    $cur = $byPid[$t.Pid]
    if (-not $cur -or $cur.CreationDate -ne $t.Created) { $tracked.Remove($key); continue }
    if ($codexLive.ContainsKey($t.OwnerKey)) { $t.SeenOrphan = 0; continue }
    $t.SeenOrphan++
    if ($t.SeenOrphan -lt 2) { $pending++; continue }
    try {
      Stop-Process -Id $t.Pid -Force -ErrorAction Stop
      $tracked.Remove($key)
      $killed++
      $script:killedTotal++
    } catch {
      $pending++
    }
  }

  $mem = Get-CimInstance Win32_OperatingSystem
  $usedMb = [math]::Round(($mem.TotalVisibleMemorySize - $mem.FreePhysicalMemory) / 1024)
  $totalMb = [math]::Round($mem.TotalVisibleMemorySize / 1024)
  $percent = [math]::Round($usedMb * 100 / $totalMb)
  $text = "RAM $percent% ($usedMb/$totalMb MB), Codex $($codexLive.Count), attached $liveAttached, pending $pending, killed now $killed, total $script:killedTotal."
  $script:statusLabel.Text = $text
  $script:tray.Text = if ($text.Length -gt 63) { $text.Substring(0, 63) } else { $text }
  Write-GuardLog $text
}

$form = New-Object Windows.Forms.Form
$form.Text = 'Codex Process Guard'
$form.Size = New-Object Drawing.Size(760, 420)
$form.StartPosition = 'CenterScreen'

$statusLabel = New-Object Windows.Forms.Label
$statusLabel.AutoSize = $true
$statusLabel.Location = New-Object Drawing.Point(12, 15)
$form.Controls.Add($statusLabel)

$cleanButton = New-Object Windows.Forms.Button
$cleanButton.Text = 'Clean now'
$cleanButton.Location = New-Object Drawing.Point(12, 45)
$cleanButton.Add_Click({ Scan-Guard })
$form.Controls.Add($cleanButton)

$hideButton = New-Object Windows.Forms.Button
$hideButton.Text = 'Hide to tray'
$hideButton.Location = New-Object Drawing.Point(105, 45)
$hideButton.Add_Click({ $form.Hide() })
$form.Controls.Add($hideButton)

$logBox = New-Object Windows.Forms.TextBox
$logBox.Multiline = $true
$logBox.ScrollBars = 'Vertical'
$logBox.ReadOnly = $true
$logBox.Location = New-Object Drawing.Point(12, 82)
$logBox.Size = New-Object Drawing.Size(720, 285)
$form.Controls.Add($logBox)

$menu = New-Object Windows.Forms.ContextMenuStrip
$openItem = $menu.Items.Add('Open')
$scanItem = $menu.Items.Add('Clean now')
$exitItem = $menu.Items.Add('Exit')
$openItem.Add_Click({ $form.Show(); $form.WindowState = 'Normal'; $form.Activate() })
$scanItem.Add_Click({ Scan-Guard })
$exitItem.Add_Click({ $script:tray.Visible = $false; $timer.Stop(); $form.Close() })

$tray = New-Object Windows.Forms.NotifyIcon
$tray.Icon = [Drawing.SystemIcons]::Information
$tray.Text = 'Codex Process Guard'
$tray.ContextMenuStrip = $menu
$tray.Visible = $true
$tray.Add_DoubleClick({ $form.Show(); $form.Activate() })

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 120000
$timer.Add_Tick({ Scan-Guard })
$timer.Start()

Write-GuardLog 'PowerShell guard started.'
Scan-Guard
if (-not $Background) { $form.Show() }
[Windows.Forms.Application]::Run()
