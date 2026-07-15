$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $root 'bin'
$csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"

New-Item -ItemType Directory -Force -Path $out | Out-Null
& $csc /nologo /target:winexe /optimize+ /platform:anycpu `
    /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll `
    /reference:System.Windows.Forms.dll /reference:System.Management.dll `
    /out:"$out\CodexProcessGuard.exe" "$root\Program.cs"
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }

try {
    $test = Start-Process -FilePath "$out\CodexProcessGuard.exe" -ArgumentList '--self-test' -Wait -PassThru -ErrorAction Stop
}
catch {
    throw "Built successfully, but Windows refused to start the safety self-test. If Smart App Control is on, use a trusted code-signing certificate; do not disable system protection. $($_.Exception.Message)"
}
if ($test.ExitCode -ne 0) { throw 'Safety self-tests failed.' }

Write-Host "Built and tested: $out\CodexProcessGuard.exe"
