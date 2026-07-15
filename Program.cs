using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Management;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace CodexProcessGuard
{
    internal static class Program
    {
        private const string MutexName = "Local\\CodexProcessGuard";

        [STAThread]
        private static int Main(string[] args)
        {
            if (args.Any(a => string.Equals(a, "--self-test", StringComparison.OrdinalIgnoreCase)))
                return SelfTests.Run();

            bool created;
            using (var mutex = new Mutex(true, MutexName, out created))
            {
                if (!created)
                {
                    MessageBox.Show("Codex Process Guard is already running. Open it from the tray icon.",
                        "Codex Process Guard", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return 0;
                }

                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new GuardContext(args.Any(a => string.Equals(a, "/background", StringComparison.OrdinalIgnoreCase))));
                GC.KeepAlive(mutex);
            }
            return 0;
        }
    }

    internal sealed class GuardContext : ApplicationContext
    {
        private readonly NotifyIcon tray;
        private readonly System.Windows.Forms.Timer timer;
        private readonly SettingsStore settingsStore;
        private readonly ProcessCleaner cleaner;
        private readonly GuardForm form;
        private readonly ScanGate scanGate = new ScanGate();
        private DateTime lastPressureNoticeUtc = DateTime.MinValue;
        private bool exiting;

        public GuardContext(bool background)
        {
            settingsStore = new SettingsStore();
            cleaner = new ProcessCleaner(settingsStore.DataDirectory);
            form = new GuardForm(settingsStore.Settings);
            form.SaveRequested += SaveSettings;
            form.CleanRequested += delegate { RunScan(true); };
            form.HideRequested += delegate { form.Hide(); };

            var menu = new ContextMenuStrip();
            menu.Items.Add("Open settings", null, delegate { ShowForm(); });
            menu.Items.Add("Clean now", null, delegate { RunScan(true); });
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add("Exit", null, delegate { ExitGuard(); });

            tray = new NotifyIcon();
            tray.Icon = SystemIcons.Information;
            tray.Text = "Codex Process Guard";
            tray.Visible = true;
            tray.ContextMenuStrip = menu;
            tray.DoubleClick += delegate { ShowForm(); };

            timer = new System.Windows.Forms.Timer();
            timer.Tick += delegate { RunScan(false); };
            ApplyInterval();
            settingsStore.ApplyStartup();

            form.SetLog(settingsStore.ReadRecentLog());
            Log("Guard started in safe mode. Active Codex process trees will never be killed.");
            RunScan(false);
            if (!background) ShowForm();
        }

        private void ShowForm()
        {
            if (form.Visible)
            {
                form.Activate();
                return;
            }
            form.Show();
            form.WindowState = FormWindowState.Normal;
            form.Activate();
        }

        private void SaveSettings(int intervalMinutes, bool startWithWindows)
        {
            settingsStore.Settings.IntervalMinutes = Math.Max(1, Math.Min(60, intervalMinutes));
            settingsStore.Settings.StartWithWindows = startWithWindows;
            settingsStore.Save();
            settingsStore.ApplyStartup();
            ApplyInterval();
            Log(string.Format("Settings saved: scan every {0} minute(s).", settingsStore.Settings.IntervalMinutes));
        }

        private void ApplyInterval()
        {
            timer.Interval = settingsStore.Settings.IntervalMinutes * 60 * 1000;
            timer.Stop();
            timer.Start();
            form.SetNextScan(DateTime.Now.AddMilliseconds(timer.Interval));
        }

        private void RunScan(bool manual)
        {
            if (!scanGate.TryEnter(manual))
            {
                if (manual) Log("A scan is already running; one follow-up scan was queued.");
                return;
            }

            form.SetScanning();
            Task.Factory.StartNew(() => cleaner.Scan())
                .ContinueWith(task =>
                {
                    bool runQueued = scanGate.ExitAndTakeQueued();
                    if (task.IsFaulted)
                    {
                        Log("Scan failed safely; nothing was killed.");
                        form.SetError();
                    }
                    else
                    {
                        var result = task.Result;
                        form.SetResult(result, DateTime.Now.AddMilliseconds(timer.Interval));
                        Log(string.Format("Scan: memory {0}%, {1} Codex root(s), {2} live-attached helper(s), {3} pending orphan(s), {4} killed.",
                            result.MemoryLoadPercent, result.LiveCodexRoots, result.LiveAttached, result.PendingOrphans, result.Killed));
                        if (result.Killed > 0)
                        {
                            tray.BalloonTipTitle = "Codex Process Guard";
                            tray.BalloonTipText = string.Format("Removed {0} orphaned Codex helper process(es).", result.Killed);
                            tray.ShowBalloonTip(4000);
                        }
                        else if (PressurePolicy.ShouldWarn(result.MemoryLoadPercent, result.LiveAttached) &&
                                 DateTime.UtcNow - lastPressureNoticeUtc >= TimeSpan.FromMinutes(30))
                        {
                            lastPressureNoticeUtc = DateTime.UtcNow;
                            tray.BalloonTipTitle = "High memory; live Codex preserved";
                            tray.BalloonTipText = string.Format(
                                "RAM is {0}%. {1} helper(s) are still attached to a running Codex and were not touched. Fully exit Codex to let Guard clean them safely.",
                                result.MemoryLoadPercent, result.LiveAttached);
                            tray.ShowBalloonTip(8000);
                        }
                    }
                    if (runQueued) RunScan(true);
                }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        private void Log(string message)
        {
            string line = string.Format("[{0:yyyy-MM-dd HH:mm:ss}] {1}", DateTime.Now, message);
            settingsStore.AppendLog(line);
            form.AppendLog(line);
        }

        private void ExitGuard()
        {
            exiting = true;
            timer.Stop();
            tray.Visible = false;
            tray.Dispose();
            form.AllowClose();
            form.Close();
            ExitThread();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing && !exiting)
            {
                timer.Dispose();
                tray.Dispose();
                form.Dispose();
            }
            base.Dispose(disposing);
        }
    }

    internal sealed class ScanGate
    {
        private int scanning;
        private int queued;

        public bool TryEnter(bool queueIfBusy)
        {
            if (Interlocked.CompareExchange(ref scanning, 1, 0) == 0) return true;
            if (queueIfBusy) Interlocked.Exchange(ref queued, 1);
            return false;
        }

        public bool ExitAndTakeQueued()
        {
            Interlocked.Exchange(ref scanning, 0);
            return Interlocked.Exchange(ref queued, 0) != 0;
        }
    }

    internal sealed class GuardForm : Form
    {
        private readonly NumericUpDown interval;
        private readonly CheckBox startup;
        private readonly Label status;
        private readonly Label nextScan;
        private readonly DataGridView cohorts;
        private readonly TextBox log;
        private bool allowClose;

        public event Action<int, bool> SaveRequested;
        public event Action CleanRequested;
        public event Action HideRequested;

        public GuardForm(GuardSettings settings)
        {
            Text = "Codex Process Guard";
            Icon = SystemIcons.Information;
            Width = 1160;
            Height = 720;
            MinimumSize = new Size(920, 600);
            StartPosition = FormStartPosition.CenterScreen;

            var title = new Label { Text = "Codex Process Guard", Font = new Font(Font, FontStyle.Bold), AutoSize = true, Left = 18, Top = 16 };
            var safety = new Label
            {
                Text = "Safe mode: never kills a helper while its exact Codex owner is running. After Codex exits, the same orphan must be confirmed in two scans before removal.",
                Left = 18, Top = 45, Width = 1090, Height = 42
            };

            var intervalLabel = new Label { Text = "Check every", Left = 18, Top = 96, AutoSize = true };
            interval = new NumericUpDown { Minimum = 1, Maximum = 60, Value = settings.IntervalMinutes, Left = 100, Top = 92, Width = 65 };
            var minutes = new Label { Text = "minute(s)", Left = 174, Top = 96, AutoSize = true };
            startup = new CheckBox { Text = "Start with Windows", Checked = settings.StartWithWindows, Left = 270, Top = 94, AutoSize = true };

            var save = new Button { Text = "Save", Left = 18, Top = 130, Width = 90 };
            var clean = new Button { Text = "Clean now", Left = 116, Top = 130, Width = 105 };
            var hide = new Button { Text = "Hide to tray", Left = 229, Top = 130, Width = 110 };
            save.Click += delegate { if (SaveRequested != null) SaveRequested((int)interval.Value, startup.Checked); };
            clean.Click += delegate { if (CleanRequested != null) CleanRequested(); };
            hide.Click += delegate { if (HideRequested != null) HideRequested(); };

            status = new Label { Text = "Status: starting...", Left = 18, Top = 178, Width = 1090, AutoEllipsis = true };
            nextScan = new Label { Text = "Next scan: —", Left = 18, Top = 202, Width = 565 };
            var cohortLabel = new Label { Text = "Managed process cohorts (read-only; live cohorts are preserved)", Left = 18, Top = 230, AutoSize = true };
            cohorts = new DataGridView
            {
                Left = 18, Top = 252, Width = 1090, Height = 245, ReadOnly = true,
                AllowUserToAddRows = false, AllowUserToDeleteRows = false, AllowUserToResizeRows = false,
                AutoGenerateColumns = false, MultiSelect = false, RowHeadersVisible = false,
                SelectionMode = DataGridViewSelectionMode.FullRowSelect,
                Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right,
                BackgroundColor = SystemColors.Window
            };
            AddColumn("Started", "Started", 76);
            AddColumn("Age", "Age", 66);
            AddColumn("Owner", "Owner PID", 68);
            AddColumn("Processes", "Proc", 48);
            AddColumn("Ram", "RAM MB", 66);
            AddColumn("Sockets", "TCP", 45);
            AddColumn("Types", "Process types", 185);
            AddColumn("Services", "MCP groups", 300, DataGridViewAutoSizeColumnMode.Fill);
            AddColumn("Decision", "Safety decision", 205);
            var logLabel = new Label { Text = "Recent safe-scan log", Left = 18, Top = 510, AutoSize = true };
            log = new TextBox
            {
                Left = 18, Top = 532, Width = 1090, Height = 135, Multiline = true, ReadOnly = true,
                ScrollBars = ScrollBars.Vertical, Font = new Font("Consolas", 9), Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right
            };

            Controls.AddRange(new Control[] { title, safety, intervalLabel, interval, minutes, startup, save, clean, hide, status, nextScan, cohortLabel, cohorts, logLabel, log });
            FormClosing += OnFormClosing;
        }

        private void AddColumn(string name, string header, int width, DataGridViewAutoSizeColumnMode sizeMode = DataGridViewAutoSizeColumnMode.None)
        {
            cohorts.Columns.Add(new DataGridViewTextBoxColumn
            {
                Name = name, HeaderText = header, Width = width, AutoSizeMode = sizeMode,
                SortMode = DataGridViewColumnSortMode.NotSortable
            });
        }

        public void SetScanning() { status.Text = "Status: scanning safely..."; }
        public void SetError() { status.Text = "Status: scan failed safely; no process was killed."; }
        public void SetNextScan(DateTime value) { nextScan.Text = "Next scan: " + value.ToString("yyyy-MM-dd HH:mm:ss"); }
        public void SetResult(CleanupResult result, DateTime next)
        {
            status.ForeColor = PressurePolicy.ShouldWarn(result.MemoryLoadPercent, result.LiveAttached) ? Color.DarkOrange : SystemColors.ControlText;
            status.Text = string.Format("Status: RAM {0}%, {1} Codex root(s), {2} live-attached, {3} pending, {4} killed.",
                result.MemoryLoadPercent, result.LiveCodexRoots, result.LiveAttached, result.PendingOrphans, result.Killed);
            cohorts.Rows.Clear();
            foreach (var item in result.Cohorts)
            {
                int rowIndex = cohorts.Rows.Add(item.StartedLocal.ToString("HH:mm:ss"), FormatAge(item.StartedLocal), item.OwnerPid,
                    item.ProcessCount, item.RamMegabytes.ToString("N0"), item.SocketProcesses, item.ProcessTypes, item.Services, item.Decision);
                if (item.Decision.StartsWith("Older", StringComparison.Ordinal)) cohorts.Rows[rowIndex].DefaultCellStyle.ForeColor = Color.DarkOrange;
                if (item.Decision.StartsWith("Owner gone", StringComparison.Ordinal)) cohorts.Rows[rowIndex].DefaultCellStyle.ForeColor = Color.DarkRed;
            }
            SetNextScan(next);
        }

        private static string FormatAge(DateTime started)
        {
            TimeSpan age = DateTime.Now - started;
            if (age < TimeSpan.Zero) age = TimeSpan.Zero;
            if (age.TotalDays >= 1) return string.Format("{0}d {1}h", (int)age.TotalDays, age.Hours);
            if (age.TotalHours >= 1) return string.Format("{0}h {1}m", (int)age.TotalHours, age.Minutes);
            return string.Format("{0}m", Math.Max(0, (int)age.TotalMinutes));
        }
        public void SetLog(string value) { log.Text = value; log.SelectionStart = log.TextLength; log.ScrollToCaret(); }
        public void AppendLog(string value)
        {
            if (log.Lines.Length > 200) log.Lines = log.Lines.Skip(log.Lines.Length - 150).ToArray();
            log.AppendText((log.TextLength == 0 ? "" : Environment.NewLine) + value);
        }
        public void AllowClose() { allowClose = true; }

        private void OnFormClosing(object sender, FormClosingEventArgs e)
        {
            if (allowClose || e.CloseReason != CloseReason.UserClosing) return;
            e.Cancel = true;
            Hide();
        }
    }

    internal sealed class ProcessCleaner
    {
        private readonly string statePath;
        private readonly Dictionary<ProcessIdentity, TrackedProcess> tracked;

        public ProcessCleaner(string dataDirectory)
        {
            // Version state with classifier semantics; never trust records created by an older classifier.
            statePath = Path.Combine(dataDirectory, "tracked-v2.tsv");
            tracked = LoadState(statePath);
        }

        public CleanupResult Scan()
        {
            var snapshot = ProcessSnapshot.Read();
            var liveCodex = snapshot.Values.Where(p => p.Name == "codex.exe").ToDictionary(p => p.Identity, p => p);

            // Forget exited/reused helper PIDs before considering any cleanup.
            foreach (var key in tracked.Keys.ToList())
            {
                ProcessInfo current;
                if (!snapshot.TryGetValue(key.Pid, out current) || !current.Identity.Equals(key)) tracked.Remove(key);
            }

            // Adopt only helpers whose complete ancestry reaches a currently live Codex identity.
            foreach (var process in snapshot.Values)
            {
                if (!Classifier.IsManagedHelper(process)) continue;
                ProcessInfo owner = Lineage.FindCodexOwner(process, snapshot);
                if (owner == null) continue;
                tracked[process.Identity] = new TrackedProcess(process.Identity, owner.Identity, 0, process.Name);
            }

            int killed = 0;
            int pending = 0;
            int liveAttached = 0;
            foreach (var pair in tracked.ToList())
            {
                var item = pair.Value;
                ProcessInfo helper;
                if (!snapshot.TryGetValue(item.Helper.Pid, out helper) || !helper.Identity.Equals(item.Helper))
                {
                    tracked.Remove(pair.Key);
                    continue;
                }

                bool ownerAlive = liveCodex.ContainsKey(item.Owner);
                if (ownerAlive)
                {
                    liveAttached++;
                    OrphanPolicy.ShouldTerminate(item, true);
                    continue;
                }

                pending++;
                if (!OrphanPolicy.ShouldTerminate(item, false)) continue;

                // Native checks below revalidate both exact identities without a slow WMI query per helper.
                IdentityStatus ownerStatus = NativeProcess.CheckIdentity(item.Owner);
                if (ownerStatus == IdentityStatus.Match)
                {
                    item.OrphanObservations = 0;
                    continue;
                }
                if (ownerStatus == IdentityStatus.Unknown) continue;

                try
                {
                    if (!NativeProcess.TryTerminate(item.Helper)) continue;
                    killed++;
                    tracked.Remove(pair.Key);
                }
                catch
                {
                    // Fail closed: access denied or a race means skip and try again later.
                }
            }

            SaveState(statePath, tracked);
            var cohorts = CohortBuilder.Build(tracked, snapshot, liveCodex, NetworkSnapshot.ReadOwningPids());
            return new CleanupResult(SystemMemory.GetLoadPercent(), liveCodex.Count, tracked.Count, liveAttached, pending, killed, cohorts);
        }

        private static Dictionary<ProcessIdentity, TrackedProcess> LoadState(string path)
        {
            var result = new Dictionary<ProcessIdentity, TrackedProcess>();
            if (!File.Exists(path)) return result;
            try
            {
                foreach (string line in File.ReadAllLines(path))
                {
                    string[] p = line.Split('\t');
                    int helperPid, ownerPid, observations;
                    long helperStart, ownerStart;
                    if (p.Length != 6 || !int.TryParse(p[0], out helperPid) || !long.TryParse(p[1], out helperStart) ||
                        !int.TryParse(p[2], out ownerPid) || !long.TryParse(p[3], out ownerStart) || !int.TryParse(p[4], out observations)) continue;
                    var item = new TrackedProcess(new ProcessIdentity(helperPid, helperStart), new ProcessIdentity(ownerPid, ownerStart), observations, p[5]);
                    result[item.Helper] = item;
                }
            }
            catch { result.Clear(); }
            return result;
        }

        private static void SaveState(string path, Dictionary<ProcessIdentity, TrackedProcess> values)
        {
            string temp = path + ".tmp";
            File.WriteAllLines(temp, values.Values.Select(v => string.Join("\t", v.Helper.Pid, v.Helper.StartTicks, v.Owner.Pid, v.Owner.StartTicks, v.OrphanObservations, v.Name)).ToArray());
            if (File.Exists(path)) File.Delete(path);
            File.Move(temp, path);
        }
    }

    internal static class Classifier
    {
        private static readonly HashSet<string> AllowedNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        { "node.exe", "node_repl.exe", "python.exe", "pythonw.exe", "uv.exe", "uvx.exe", "npm.exe", "npx.exe", "cmd.exe" };

        public static bool IsManagedHelper(ProcessInfo process)
        {
            return GetServiceName(process) != null;
        }

        public static string GetServiceName(ProcessInfo process)
        {
            if (!AllowedNames.Contains(process.Name)) return null;
            if (process.Name.Equals("node_repl.exe", StringComparison.OrdinalIgnoreCase)) return "Node REPL";
            string command = (process.CommandLine ?? "").ToLowerInvariant();
            if (command.Contains("mongodb-mcp-server")) return "MongoDB";
            if (command.Contains("figma-developer-mcp")) return "Figma";
            if (command.Contains("@coding-solo\\godot-mcp") || command.Contains("@coding-solo/godot-mcp") || command.Contains("godot_visual_mcp_server")) return "Godot";
            if (command.Contains("@sentry\\mcp-server")) return "Sentry";
            if (command.Contains("observability-mcp")) return "Observability";
            if (command.Contains("mcp-atlassian")) return "Atlassian";
            if (command.Contains("sonar-mcp")) return "Sonar";
            if (command.Contains("mcp-server-postgres")) return "Postgres";
            if (command.Contains("mcp-server-memory")) return "Memory";
            if (command.Contains("mcp-server-gitlab")) return "GitLab";
            if (command.Contains("codex_apps")) return "Codex Apps";
            if (command.Contains("@modelcontextprotocol")) return "Model Context";
            if (command.Contains("\\mcp\\server.mjs") || command.Contains("/mcp/server.mjs")) return "Custom MCP";
            if (command.Contains("\\.codex\\tools\\")) return "Codex tool";
            if (command.Contains("\\.codex\\plugins\\")) return "Codex plugin";
            return null;
        }
    }

    internal static class CohortBuilder
    {
        private static readonly long CohortWindowTicks = TimeSpan.FromSeconds(8).Ticks;

        public static List<CohortInfo> Build(Dictionary<ProcessIdentity, TrackedProcess> tracked,
            Dictionary<int, ProcessInfo> snapshot, Dictionary<ProcessIdentity, ProcessInfo> liveCodex, HashSet<int> socketPids)
        {
            var result = new List<CohortInfo>();
            var valid = tracked.Values.Where(item =>
            {
                ProcessInfo process;
                return snapshot.TryGetValue(item.Helper.Pid, out process) && process.Identity.Equals(item.Helper);
            });

            foreach (var ownerGroup in valid.GroupBy(item => item.Owner))
            {
                var ordered = ownerGroup.OrderBy(item => item.Helper.StartTicks).ToList();
                var groups = new List<List<TrackedProcess>>();
                foreach (var item in ordered)
                {
                    if (groups.Count == 0 || item.Helper.StartTicks - groups[groups.Count - 1][0].Helper.StartTicks > CohortWindowTicks)
                        groups.Add(new List<TrackedProcess>());
                    groups[groups.Count - 1].Add(item);
                }

                bool ownerAlive = liveCodex.ContainsKey(ownerGroup.Key);
                for (int i = 0; i < groups.Count; i++)
                {
                    var members = groups[i];
                    var processes = members.Select(item => snapshot[item.Helper.Pid]).ToList();
                    bool newest = i == groups.Count - 1;
                    string decision;
                    if (!ownerAlive)
                        decision = string.Format("Owner gone; orphan checks {0}/2", Math.Min(2, members.Max(item => item.OrphanObservations)));
                    else if (newest)
                        decision = "Newest live cohort - preserved";
                    else
                        decision = "Older live cohort - preserved";

                    string types = string.Join(", ", processes
                        .GroupBy(process => DisplayProcessType(process.Name))
                        .OrderByDescending(group => group.Count())
                        .ThenBy(group => group.Key)
                        .Select(group => string.Format("{0} {1}", group.Key, group.Count())).ToArray());
                    string services = string.Join(", ", processes
                        .GroupBy(process => Classifier.GetServiceName(process) ?? "Unknown")
                        .OrderByDescending(group => group.Count())
                        .ThenBy(group => group.Key)
                        .Select(group => string.Format("{0} {1}", group.Key, group.Count())).ToArray());

                    result.Add(new CohortInfo(
                        new DateTime(members[0].Helper.StartTicks, DateTimeKind.Utc).ToLocalTime(),
                        ownerGroup.Key.Pid,
                        members.Count,
                        processes.Sum(process => process.WorkingSetBytes) / 1024d / 1024d,
                        processes.Count(process => socketPids.Contains(process.Id)),
                        types, services, decision));
                }
            }
            return result.OrderByDescending(item => item.StartedLocal).ToList();
        }

        private static string DisplayProcessType(string name)
        {
            if (name == "python.exe" || name == "pythonw.exe") return "python";
            if (name == "uv.exe" || name == "uvx.exe") return "uv";
            return name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? name.Substring(0, name.Length - 4) : name;
        }
    }

    internal static class NetworkSnapshot
    {
        public static HashSet<int> ReadOwningPids()
        {
            var result = new HashSet<int>();
            try
            {
                using (var searcher = new ManagementObjectSearcher(@"root\StandardCimv2", "SELECT OwningProcess FROM MSFT_NetTCPConnection"))
                using (var objects = searcher.Get())
                {
                    foreach (ManagementObject item in objects)
                    using (item) result.Add(Convert.ToInt32(item["OwningProcess"], CultureInfo.InvariantCulture));
                }
            }
            catch { }
            return result;
        }
    }

    internal static class OrphanPolicy
    {
        public static bool ShouldTerminate(TrackedProcess item, bool ownerAlive)
        {
            if (ownerAlive)
            {
                item.OrphanObservations = 0;
                return false;
            }
            item.OrphanObservations++;
            return item.OrphanObservations >= 2;
        }
    }

    internal static class PressurePolicy
    {
        public static bool ShouldWarn(int memoryLoadPercent, int liveAttached)
        {
            return memoryLoadPercent >= 85 && liveAttached > 0;
        }
    }

    internal static class SystemMemory
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private sealed class MemoryStatus
        {
            public uint Length = (uint)Marshal.SizeOf(typeof(MemoryStatus));
            public uint MemoryLoad;
            public ulong TotalPhysical;
            public ulong AvailablePhysical;
            public ulong TotalPageFile;
            public ulong AvailablePageFile;
            public ulong TotalVirtual;
            public ulong AvailableVirtual;
            public ulong AvailableExtendedVirtual;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GlobalMemoryStatusEx([In, Out] MemoryStatus status);

        public static int GetLoadPercent()
        {
            var status = new MemoryStatus();
            return GlobalMemoryStatusEx(status) ? (int)status.MemoryLoad : 0;
        }
    }

    internal static class Lineage
    {
        public static ProcessInfo FindCodexOwner(ProcessInfo process, Dictionary<int, ProcessInfo> snapshot)
        {
            var seen = new HashSet<int>();
            ProcessInfo current = process;
            for (int i = 0; i < 64 && current != null && seen.Add(current.Id); i++)
            {
                if (current.Name == "codex.exe") return current;
                ProcessInfo parent;
                if (!snapshot.TryGetValue(current.ParentId, out parent)) return null;
                if (parent.StartTicks > current.StartTicks) return null; // Parent PID was reused.
                current = parent;
            }
            return null;
        }
    }

    internal static class ProcessSnapshot
    {
        public static Dictionary<int, ProcessInfo> Read()
        {
            var result = new Dictionary<int, ProcessInfo>();
            using (var searcher = new ManagementObjectSearcher("SELECT ProcessId, ParentProcessId, Name, CommandLine, CreationDate, WorkingSetSize FROM Win32_Process"))
            using (var objects = searcher.Get())
            {
                foreach (ManagementObject item in objects)
                using (item)
                {
                    var process = FromManagementObject(item);
                    if (process != null) result[process.Id] = process;
                }
            }
            return result;
        }

        private static ProcessInfo FromManagementObject(ManagementObject item)
        {
            try
            {
                int id = Convert.ToInt32(item["ProcessId"], CultureInfo.InvariantCulture);
                int parent = Convert.ToInt32(item["ParentProcessId"], CultureInfo.InvariantCulture);
                string name = Convert.ToString(item["Name"], CultureInfo.InvariantCulture).ToLowerInvariant();
                string command = Convert.ToString(item["CommandLine"], CultureInfo.InvariantCulture);
                string creation = Convert.ToString(item["CreationDate"], CultureInfo.InvariantCulture);
                long ticks = ManagementDateTimeConverter.ToDateTime(creation).ToUniversalTime().Ticks;
                long workingSet = item["WorkingSetSize"] == null ? 0 : Convert.ToInt64(item["WorkingSetSize"], CultureInfo.InvariantCulture);
                return new ProcessInfo(id, parent, name, command, ticks, workingSet);
            }
            catch { return null; }
        }
    }

    internal static class NativeProcess
    {
        private const uint ProcessTerminate = 0x0001;
        private const uint ProcessQueryLimitedInformation = 0x1000;
        private const int ErrorInvalidParameter = 87;

        [StructLayout(LayoutKind.Sequential)]
        private struct FileTime { public uint Low; public uint High; }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(uint access, bool inheritHandle, int processId);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetProcessTimes(IntPtr process, out FileTime creation, out FileTime exit, out FileTime kernel, out FileTime user);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool TerminateProcess(IntPtr process, uint exitCode);
        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr handle);

        public static IdentityStatus CheckIdentity(ProcessIdentity expected)
        {
            IntPtr handle = OpenProcess(ProcessQueryLimitedInformation, false, expected.Pid);
            if (handle == IntPtr.Zero)
                return Marshal.GetLastWin32Error() == ErrorInvalidParameter ? IdentityStatus.Missing : IdentityStatus.Unknown;
            try
            {
                FileTime creation, exit, kernel, user;
                if (!GetProcessTimes(handle, out creation, out exit, out kernel, out user)) return IdentityStatus.Unknown;
                if (exit.Low != 0 || exit.High != 0) return IdentityStatus.Missing;
                long fileTime = ((long)creation.High << 32) | creation.Low;
                long actualTicks = DateTime.FromFileTimeUtc(fileTime).Ticks;
                return NormalizeTicks(actualTicks) == NormalizeTicks(expected.StartTicks) ? IdentityStatus.Match : IdentityStatus.Different;
            }
            finally { CloseHandle(handle); }
        }

        public static bool TryTerminate(ProcessIdentity expected)
        {
            IntPtr handle = OpenProcess(ProcessTerminate | ProcessQueryLimitedInformation, false, expected.Pid);
            if (handle == IntPtr.Zero) return false;
            try
            {
                FileTime creation, exit, kernel, user;
                if (!GetProcessTimes(handle, out creation, out exit, out kernel, out user)) return false;
                long fileTime = ((long)creation.High << 32) | creation.Low;
                long actualTicks = DateTime.FromFileTimeUtc(fileTime).Ticks;
                if (NormalizeTicks(actualTicks) != NormalizeTicks(expected.StartTicks)) return false;
                return TerminateProcess(handle, 1);
            }
            finally { CloseHandle(handle); }
        }

        public static long NormalizeTicks(long ticks) { return ticks - (ticks % 10); }
    }

    internal sealed class SettingsStore
    {
        private readonly string settingsPath;
        private readonly string logPath;
        public readonly string DataDirectory;
        public GuardSettings Settings { get; private set; }

        public SettingsStore()
        {
            DataDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CodexProcessGuard");
            Directory.CreateDirectory(DataDirectory);
            settingsPath = Path.Combine(DataDirectory, "settings.ini");
            logPath = Path.Combine(DataDirectory, "guard.log");
            Settings = Load();
        }

        private GuardSettings Load()
        {
            var value = new GuardSettings { IntervalMinutes = 2, StartWithWindows = true };
            if (!File.Exists(settingsPath)) return value;
            try
            {
                foreach (string line in File.ReadAllLines(settingsPath))
                {
                    string[] p = line.Split(new[] { '=' }, 2);
                    if (p.Length != 2) continue;
                    int interval;
                    bool startup;
                    if (p[0] == "IntervalMinutes" && int.TryParse(p[1], out interval)) value.IntervalMinutes = Math.Max(1, Math.Min(60, interval));
                    if (p[0] == "StartWithWindows" && bool.TryParse(p[1], out startup)) value.StartWithWindows = startup;
                }
            }
            catch { }
            return value;
        }

        public void Save()
        {
            File.WriteAllLines(settingsPath, new[]
            {
                "IntervalMinutes=" + Settings.IntervalMinutes.ToString(CultureInfo.InvariantCulture),
                "StartWithWindows=" + Settings.StartWithWindows.ToString(CultureInfo.InvariantCulture)
            });
        }

        public void ApplyStartup()
        {
            using (var key = Registry.CurrentUser.OpenSubKey("Software\\Microsoft\\Windows\\CurrentVersion\\Run", true))
            {
                if (key == null) return;
                if (Settings.StartWithWindows) key.SetValue("CodexProcessGuard", "\"" + Application.ExecutablePath + "\" /background");
                else key.DeleteValue("CodexProcessGuard", false);
            }
        }

        public void AppendLog(string line)
        {
            try
            {
                if (File.Exists(logPath) && new FileInfo(logPath).Length > 1024 * 1024) File.Delete(logPath);
                File.AppendAllText(logPath, line + Environment.NewLine);
            }
            catch { }
        }

        public string ReadRecentLog()
        {
            try
            {
                if (!File.Exists(logPath)) return "";
                string[] lines = File.ReadAllLines(logPath);
                return string.Join(Environment.NewLine, lines.Skip(Math.Max(0, lines.Length - 100)).ToArray());
            }
            catch { return ""; }
        }
    }

    internal sealed class GuardSettings { public int IntervalMinutes; public bool StartWithWindows; }
    internal sealed class CleanupResult
    {
        public readonly int MemoryLoadPercent, LiveCodexRoots, Tracked, LiveAttached, PendingOrphans, Killed;
        public readonly List<CohortInfo> Cohorts;
        public CleanupResult(int memoryLoadPercent, int roots, int tracked, int liveAttached, int pending, int killed, List<CohortInfo> cohorts)
        {
            MemoryLoadPercent = memoryLoadPercent;
            LiveCodexRoots = roots;
            Tracked = tracked;
            LiveAttached = liveAttached;
            PendingOrphans = pending;
            Killed = killed;
            Cohorts = cohorts;
        }
    }
    internal sealed class CohortInfo
    {
        public readonly DateTime StartedLocal;
        public readonly int OwnerPid, ProcessCount, SocketProcesses;
        public readonly double RamMegabytes;
        public readonly string ProcessTypes, Services, Decision;
        public CohortInfo(DateTime startedLocal, int ownerPid, int processCount, double ramMegabytes, int socketProcesses,
            string processTypes, string services, string decision)
        {
            StartedLocal = startedLocal;
            OwnerPid = ownerPid;
            ProcessCount = processCount;
            RamMegabytes = ramMegabytes;
            SocketProcesses = socketProcesses;
            ProcessTypes = processTypes;
            Services = services;
            Decision = decision;
        }
    }
    internal sealed class ProcessInfo
    {
        public readonly int Id, ParentId; public readonly string Name, CommandLine; public readonly long StartTicks, WorkingSetBytes;
        public ProcessIdentity Identity { get { return new ProcessIdentity(Id, StartTicks); } }
        public ProcessInfo(int id, int parent, string name, string command, long start, long workingSetBytes = 0)
        {
            Id = id;
            ParentId = parent;
            Name = name;
            CommandLine = command;
            StartTicks = start;
            WorkingSetBytes = workingSetBytes;
        }
    }
    internal enum IdentityStatus { Match, Different, Missing, Unknown }
    internal struct ProcessIdentity : IEquatable<ProcessIdentity>
    {
        public readonly int Pid; public readonly long StartTicks;
        public ProcessIdentity(int pid, long startTicks) { Pid = pid; StartTicks = startTicks; }
        public bool Equals(ProcessIdentity other) { return Pid == other.Pid && StartTicks == other.StartTicks; }
        public override bool Equals(object obj) { return obj is ProcessIdentity && Equals((ProcessIdentity)obj); }
        public override int GetHashCode() { unchecked { return (Pid * 397) ^ StartTicks.GetHashCode(); } }
    }
    internal sealed class TrackedProcess
    {
        public readonly ProcessIdentity Helper, Owner; public int OrphanObservations; public readonly string Name;
        public TrackedProcess(ProcessIdentity helper, ProcessIdentity owner, int observations, string name) { Helper = helper; Owner = owner; OrphanObservations = observations; Name = name; }
    }

    internal static class SelfTests
    {
        public static int Run()
        {
            try
            {
                Assert(Classifier.IsManagedHelper(new ProcessInfo(2, 1, "node.exe", @"node C:\cache\@modelcontextprotocol\server-postgres\index.js", 20)));
                Assert(!Classifier.IsManagedHelper(new ProcessInfo(3, 1, "node.exe", "node next dev", 30)));
                Assert(!Classifier.IsManagedHelper(new ProcessInfo(4, 1, "chrome.exe", "mcp", 40)));
                Assert(!Classifier.IsManagedHelper(new ProcessInfo(5, 1, "node.exe", @"node C:\work\mcp-dashboard\server.js", 50)));
                Assert(Classifier.IsManagedHelper(new ProcessInfo(6, 1, "node.exe", @"node C:\Users\x\.codex\tools\custom\server.mjs", 60)));
                Assert(Classifier.GetServiceName(new ProcessInfo(7, 1, "node.exe", @"node C:\cache\@modelcontextprotocol\server-filesystem\index.js", 70)) == "Model Context");
                Assert(Classifier.GetServiceName(new ProcessInfo(8, 1, "node_repl.exe", "", 80)) == "Node REPL");

                var codex = new ProcessInfo(10, 1, "codex.exe", "codex app-server", 100);
                var wrapper = new ProcessInfo(11, 10, "cmd.exe", "npx mcp-server", 110);
                var helper = new ProcessInfo(12, 11, "node.exe", "node mcp-server", 120);
                var map = new Dictionary<int, ProcessInfo> { { 10, codex }, { 11, wrapper }, { 12, helper } };
                Assert(Lineage.FindCodexOwner(helper, map).Identity.Equals(codex.Identity));

                // A reused parent PID started after its alleged child must never establish ownership.
                map[10] = new ProcessInfo(10, 1, "codex.exe", "codex app-server", 999);
                Assert(Lineage.FindCodexOwner(helper, map) == null);
                Assert(NativeProcess.NormalizeTicks(123456789) == NativeProcess.NormalizeTicks(123456780));
                using (var current = Process.GetCurrentProcess())
                {
                    var currentIdentity = new ProcessIdentity(current.Id, current.StartTime.ToUniversalTime().Ticks);
                    Assert(NativeProcess.CheckIdentity(currentIdentity) == IdentityStatus.Match);
                    Assert(NativeProcess.CheckIdentity(new ProcessIdentity(current.Id, 1)) == IdentityStatus.Different);
                }
                using (var exited = Process.Start(new ProcessStartInfo
                {
                    FileName = "cmd.exe", Arguments = "/d /c exit 0", CreateNoWindow = true, UseShellExecute = false
                }))
                {
                    var exitedIdentity = new ProcessIdentity(exited.Id, exited.StartTime.ToUniversalTime().Ticks);
                    exited.WaitForExit();
                    Assert(NativeProcess.CheckIdentity(exitedIdentity) == IdentityStatus.Missing);
                }

                var tracked = new TrackedProcess(helper.Identity, codex.Identity, 1, "node.exe");
                Assert(!OrphanPolicy.ShouldTerminate(tracked, true) && tracked.OrphanObservations == 0);
                Assert(!OrphanPolicy.ShouldTerminate(tracked, false) && tracked.OrphanObservations == 1);
                Assert(OrphanPolicy.ShouldTerminate(tracked, false) && tracked.OrphanObservations == 2);
                Assert(!PressurePolicy.ShouldWarn(84, 100));
                Assert(!PressurePolicy.ShouldWarn(90, 0));
                Assert(PressurePolicy.ShouldWarn(85, 1));
                Assert(SystemMemory.GetLoadPercent() >= 0 && SystemMemory.GetLoadPercent() <= 100);

                var scanGate = new ScanGate();
                Assert(scanGate.TryEnter(false));
                Assert(!scanGate.TryEnter(true));
                Assert(scanGate.ExitAndTakeQueued());
                Assert(scanGate.TryEnter(false));
                Assert(!scanGate.ExitAndTakeQueued());

                long baseTicks = DateTime.UtcNow.AddMinutes(-30).Ticks;
                var cohortOwner = new ProcessInfo(20, 1, "codex.exe", "codex app-server", baseTicks - TimeSpan.TicksPerSecond);
                var cohortA = new ProcessInfo(21, 20, "node.exe", "mongodb-mcp-server", baseTicks, 100 * 1024 * 1024);
                var cohortB = new ProcessInfo(22, 20, "cmd.exe", "mongodb-mcp-server", baseTicks + TimeSpan.FromSeconds(2).Ticks, 10 * 1024 * 1024);
                var cohortC = new ProcessInfo(23, 20, "node.exe", "mongodb-mcp-server", baseTicks + TimeSpan.FromSeconds(12).Ticks, 120 * 1024 * 1024);
                var cohortTracked = new Dictionary<ProcessIdentity, TrackedProcess>
                {
                    { cohortA.Identity, new TrackedProcess(cohortA.Identity, cohortOwner.Identity, 0, cohortA.Name) },
                    { cohortB.Identity, new TrackedProcess(cohortB.Identity, cohortOwner.Identity, 0, cohortB.Name) },
                    { cohortC.Identity, new TrackedProcess(cohortC.Identity, cohortOwner.Identity, 0, cohortC.Name) }
                };
                var cohortSnapshot = new Dictionary<int, ProcessInfo> { { 21, cohortA }, { 22, cohortB }, { 23, cohortC } };
                var cohortOwners = new Dictionary<ProcessIdentity, ProcessInfo> { { cohortOwner.Identity, cohortOwner } };
                var builtCohorts = CohortBuilder.Build(cohortTracked, cohortSnapshot, cohortOwners, new HashSet<int> { 23 });
                Assert(builtCohorts.Count == 2);
                Assert(builtCohorts[0].ProcessCount == 1 && builtCohorts[0].SocketProcesses == 1 && builtCohorts[0].Decision.StartsWith("Newest"));
                Assert(builtCohorts[1].ProcessCount == 2 && builtCohorts[1].Decision.StartsWith("Older"));
                return 0;
            }
            catch { return 1; }
        }

        private static void Assert(bool value) { if (!value) throw new InvalidOperationException("Self-test failed."); }
    }
}
