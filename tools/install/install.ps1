# Installs Beat the Beat: Rhythm Paradise (the Qilin recompilation) on Windows.
#
# Nothing of the game is in this repository, and a built game can't be
# shared either (it is compiled from the game's own code). So the installer
# builds it here, from your own disc:
#
#   1. checks the tools (CMake, Visual Studio's C++ compiler, Python);
#   2. takes the folder of your extracted disc (PAL, SOMP01) and checks it;
#   3. recompiles the game's code and builds the game (several minutes);
#   4. puts it in its own folder, with shortcuts and an uninstaller.
#
# Run install.cmd (double-click), or:
#   powershell -ExecutionPolicy Bypass -File tools\install\install.ps1 [-Disc FOLDER] [-InstallDir FOLDER]
#       [-NoShortcuts] [-NoDesktop] [-Portable] [-Checked] [-Jobs N] [-KeepBuild]
#
# It needs the Internet once (CMake fetches SDL3, the Vulkan headers and
# Dear ImGui, each checked against a pinned hash). Works with Windows
# PowerShell 5.1 and later. Fails loudly: every step that can't be done says
# why and stops.
[CmdletBinding()]
param(
    [string]$Disc,
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\Beat the Beat - Rhythm Paradise'),
    [switch]$NoShortcuts,
    [switch]$NoDesktop,
    # Only the folder (with Play.cmd): no shortcuts, nothing in Windows' "Installed apps".
    [switch]$Portable,
    # Builds with checked memory: every access of the game's code is tested,
    # so a fault there is caught and named, at about twice the time in that
    # code (more lag on a slow computer). For reporting a crash; the default
    # is without the checks.
    [switch]$Checked,
    # The default since v0.17.0; still accepted from older command lines.
    [switch]$Unchecked,
    [int]$Jobs = 0,
    [switch]$KeepBuild
)

$ErrorActionPreference = 'Stop'
$GameName = 'Beat the Beat: Rhythm Paradise'
$ShortcutName = 'Beat the Beat - Rhythm Paradise'   # a file name: no colon
$DolSha256 = '2e0368359106a2eef773c70ef083e89aafe1e49603eefc7f16b9ed414cd098fe'   # PAL SOMP01 rev 0
$UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\BeatTheBeat-QilinRecomp'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Work = Join-Path $Repo 'build-install'
$Log = Join-Path $Work 'install.log'

# SHA-256 of a file, upper-case hex. Not Get-FileHash: that cmdlet is missing
# when Windows PowerShell inherits a PowerShell 7 module path, as it does
# when started from pwsh (or from CI's shell).
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($path))  # .NET resolves against the process folder, not PowerShell location
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}
function Say([string]$text) { Write-Host $text }
function Step([string]$text) { Write-Host ''; Write-Host "== $text" -ForegroundColor Cyan }
function Fail([string]$text) {
    Write-Host ''
    Write-Host "The installation stopped: $text" -ForegroundColor Red
    if (Test-Path $Log) { Write-Host "The full log is $Log" }
    exit 1
}

# Runs a program, its output into the log; stops with the log's end if it fails.
function Run([string]$what, [string]$exe, [string[]]$arguments) {
    Add-Content -Path $Log -Value "`r`n>>> $what`r`n>>> $exe $($arguments -join ' ')"
    # Start-Process, not the call operator: Windows PowerShell 5.1 turns a
    # native program's error output into errors of its own.
    $out = Join-Path $Work 'step.out.txt'; $err = Join-Path $Work 'step.err.txt'
    $quoted = $arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }
    $p = Start-Process -FilePath $exe -ArgumentList $quoted -NoNewWindow -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    Get-Content $out, $err -ErrorAction SilentlyContinue | Add-Content -Path $Log
    if ($p.ExitCode -ne 0) {
        Write-Host ''
        Get-Content $Log -Tail 30 | ForEach-Object { Write-Host "    $_" }
        Fail "$what failed (exit code $($p.ExitCode))."
    }
}

function Find-Built([string]$dir, [string]$name) {
    foreach ($candidate in @((Join-Path $dir "Release\$name"), (Join-Path $dir $name))) {
        if (Test-Path $candidate) { return $candidate }
    }
    Fail "$name wasn't found in $dir after building."
}

Say "$GameName - installation"
Say 'A static recompilation: the game is built on this computer from your own disc.'

# ---- 1. the tools ----
Step 'Checking the tools'
$missing = @()
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) {
    $v = (& cmake --version | Select-Object -First 1) -replace '[^\d.]', ''
    if ([version]$v -lt [version]'3.25') { $missing += "CMake 3.25 or newer (this one is $v):  winget install Kitware.CMake" }
    else { Say "  CMake $v" }
} else { $missing += 'CMake 3.25 or newer:  winget install Kitware.CMake' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = $null
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property catalog_productDisplayVersion
}
$vsHelp = 'Visual Studio 2022 (17.10 or newer) with "Desktop development with C++":  winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"'
if ($vs) {
    $vsVersion = [version](($vs -split '[^\d.]')[0])
    if ($vsVersion -lt [version]'17.10') { $missing += "$vsHelp  (this one is $vs)" } else { Say "  Visual Studio C++ $vs" }
} else { $missing += $vsHelp }

$python = Get-Command python -ErrorAction SilentlyContinue
$pythonOk = $false
if ($python) {
    $pv = (& python --version 2>&1) -replace '[^\d.]', ''
    if ($pv -and [version]$pv -ge [version]'3.9') { $pythonOk = $true; Say "  Python $pv" }
}
if (-not $pythonOk) { $missing += 'Python 3.9 or newer:  winget install Python.Python.3.12' }

if ($missing.Count) {
    Write-Host ''
    Write-Host 'These are needed first (each line ends with a command that installs it):' -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "  - $_" }
    Fail 'install them, open a new window and run this again.'
}
$drive = (Get-Item $Repo).PSDrive
if ($drive.Free -lt 6GB) { Fail ("the build needs about 6 GB free on drive {0}: (it has {1:N1} GB)." -f $drive.Name, ($drive.Free / 1GB)) }

# ---- 2. the disc ----
Step 'Your disc'
if (-not $Disc) {
    Say '  Choose the folder of your extracted disc (Dolphin: right-click the game, Properties,'
    Say '  Filesystem, right-click "Disc", Extract Entire Disc). It holds "sys" and "files".'
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = 'The folder of your extracted Beat the Beat: Rhythm Paradise disc (it holds "sys" and "files")'
        $dialog.ShowNewFolderButton = $false
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $Disc = $dialog.SelectedPath }
    } catch { }
    if (-not $Disc) { $Disc = Read-Host '  The folder (or Enter to stop)' }
    if (-not $Disc) { Fail 'no disc folder was given.' }
}
$Disc = $Disc.Trim('"')
if (Test-Path $Disc -PathType Leaf) {
    Fail "$Disc is a file. The game is read from an extracted disc folder: a .wbfs, .iso or .rvz must be extracted first (Dolphin: Extract Entire Disc)."
}
if (-not (Test-Path $Disc -PathType Container)) { Fail "$Disc isn't a folder." }
$Disc = (Resolve-Path $Disc).Path
# Chosen one level too deep ("sys" or "files", or a partition folder above)?
foreach ($candidate in @($Disc, (Split-Path $Disc -Parent), (Join-Path $Disc 'DATA'))) {
    if ($candidate -and (Test-Path (Join-Path $candidate 'sys\main.dol')) -and (Test-Path (Join-Path $candidate 'files'))) { $Disc = $candidate; break }
}
$Dol = Join-Path $Disc 'sys\main.dol'
if (-not (Test-Path $Dol)) { Fail "$Disc has no sys\main.dol: it isn't an extracted Wii disc (it should hold ""sys"" and ""files"")." }
if (-not (Test-Path (Join-Path $Disc 'files'))) { Fail "$Disc has no ""files"" folder: the whole disc must be extracted, not only its system files." }
$hash = (Get-Sha256 $Dol).ToLower()
if ($hash -ne $DolSha256) {
    Fail "this isn't the supported disc. It must be the European (PAL) Beat the Beat: Rhythm Paradise, SOMP01 revision 0; other regions and revisions aren't supported yet. (main.dol's SHA-256 is $hash.)"
}
Say "  $Disc"
Say '  The disc is the supported one (PAL, SOMP01).'

# ---- 3. the build ----
New-Item -ItemType Directory -Force $Work | Out-Null
Set-Content -Path $Log -Value "$GameName installation, $(Get-Date -Format s)"
$parallel = @('--parallel'); if ($Jobs -gt 0) { $parallel = @('--parallel', "$Jobs") }
$tools = Join-Path $Work 'tools'; $game = Join-Path $Work 'game'; $gen = Join-Path $Work 'gen-rt'; $symbols = Join-Path $Work 'pal_symbols.txt'

Step 'Building the recompiler (1 of 4)'
Run 'Configuring the recompiler' 'cmake' @('-S', $Repo, '-B', $tools, '-DCMAKE_BUILD_TYPE=Release')
Run 'Building the recompiler' 'cmake' (@('--build', $tools, '--config', 'Release', '--target', 'qilin') + $parallel)
$qilin = Find-Built $tools 'qilin.exe'

Step "Reading the game's code (2 of 4)"
Run 'Naming the functions' 'python' @((Join-Path $Repo 'tools\analysis\port_symbols.py'), '--qilin', $qilin, '--dol', $Dol, '--symbols-out', $symbols)
Run "Recompiling the game's code to C++" $qilin @('lift', $Dol, '--out', $gen, '--runtime', '--symbols', $symbols)

Step 'Building the game (3 of 4): this is the long step, several minutes'
$configure = @('-S', $Repo, '-B', $game, '-DCMAKE_BUILD_TYPE=Release', "-DQILIN_RUNTIME_GEN_DIR=$gen")
if ($Checked -and $Unchecked) { throw '-Checked and -Unchecked contradict each other: give one.' }
if (-not $Checked) { $configure += '-DQILIN_RUNTIME_CHECKED=OFF' }
else { $configure += '-DQILIN_RUNTIME_CHECKED=ON' }   # explicit: a kept build folder may hold OFF from before
Run 'Configuring the game (fetches SDL3, the Vulkan headers and Dear ImGui)' 'cmake' $configure
Run 'Building the game' 'cmake' (@('--build', $game, '--config', 'Release', '--target', 'qilin_run') + $parallel)
$built = Find-Built $game 'qilin_run.exe'

# ---- 4. the installation ----
Step 'Installing (4 of 4)'
$version = '0'
$m = Select-String -Path (Join-Path $Repo 'CMakeLists.txt') -Pattern 'project\([^)]*VERSION\s+([\d.]+)' | Select-Object -First 1
if ($m) { $version = $m.Matches[0].Groups[1].Value }
New-Item -ItemType Directory -Force $InstallDir | Out-Null
$exe = Join-Path $InstallDir 'qilin_run.exe'
Copy-Item $built $exe -Force
Copy-Item (Join-Path $PSScriptRoot 'uninstall.ps1') (Join-Path $InstallDir 'uninstall.ps1') -Force
Set-Content -Path (Join-Path $InstallDir 'Uninstall.cmd') -Encoding ASCII -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File ""%~dp0uninstall.ps1"" %*`r`npause"
# The game is started with the disc's folder: a launcher beside it, and the shortcuts.
Set-Content -Path (Join-Path $InstallDir 'Play.cmd') -Encoding ASCII -Value "@echo off`r`nstart """" ""%~dp0qilin_run.exe"" ""$Disc"""
Set-Content -Path (Join-Path $InstallDir 'install-info.txt') -Value @(
    "$GameName (Qilin recompilation) $version",
    "Installed $(Get-Date -Format 'yyyy-MM-dd HH:mm') from $Repo",
    "Disc folder: $Disc",
    "Memory checks: $(if ($Checked) { 'on (-Checked)' } else { 'off' })",
    'Saves and settings: %LOCALAPPDATA%\qilin\beatthebeat (kept when uninstalling, unless asked)')

$made = @()
if (-not $NoShortcuts -and -not $Portable) {
    $shell = New-Object -ComObject WScript.Shell
    $places = @([Environment]::GetFolderPath('Programs'))
    if (-not $NoDesktop) { $places += [Environment]::GetFolderPath('Desktop') }
    foreach ($place in $places) {
        $link = Join-Path $place "$ShortcutName.lnk"
        $s = $shell.CreateShortcut($link)
        $s.TargetPath = $exe
        $s.Arguments = '"' + $Disc + '"'
        $s.WorkingDirectory = $InstallDir
        $s.Description = $GameName
        $s.Save()
        $made += $link
    }
}
Set-Content -Path (Join-Path $InstallDir 'shortcuts.txt') -Value $made

# "Installed apps" in Windows' settings (for this user; no administrator needed).
if (-not $Portable) {
    New-Item -Path $UninstallKey -Force | Out-Null
    Set-ItemProperty $UninstallKey -Name DisplayName -Value "$GameName (Qilin recompilation)"
    Set-ItemProperty $UninstallKey -Name DisplayVersion -Value $version
    Set-ItemProperty $UninstallKey -Name Publisher -Value 'Qilin'
    Set-ItemProperty $UninstallKey -Name InstallLocation -Value $InstallDir
    Set-ItemProperty $UninstallKey -Name UninstallString -Value ('powershell -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $InstallDir 'uninstall.ps1') + '"')
    Set-ItemProperty $UninstallKey -Name NoModify -Value 1 -Type DWord
    Set-ItemProperty $UninstallKey -Name NoRepair -Value 1 -Type DWord
    Set-ItemProperty $UninstallKey -Name EstimatedSize -Value ([int]((Get-Item $exe).Length / 1KB)) -Type DWord
}

if (-not $KeepBuild) {
    # The build folder is several GB and only needed to build again.
    Remove-Item -Recurse -Force $tools, $game, $gen -ErrorAction SilentlyContinue
    Remove-Item $symbols, (Join-Path $Work 'step.out.txt'), (Join-Path $Work 'step.err.txt') -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host "$GameName is installed." -ForegroundColor Green
Say "  In:  $InstallDir"
if ($made.Count) {
    $where = 'the Start menu'; if (-not $NoDesktop) { $where = 'the Start menu or the desktop' }
    Say ("  Start it from $where (""$ShortcutName"").")
} else { Say ("  Start it with:  ""$(Join-Path $InstallDir 'Play.cmd')""") }
Say '  Keys: A = A, Right Shift or Right arrow; B = B, Left Shift or Left arrow; + = Enter, - = Backspace, Home = H, d-pad = I J K L; A and B together on the title.'
Say '  F1 or Esc opens the settings; a controller plays too (any gamepad).'
Say '  The disc folder must stay where it is: the game reads its files from it.'
if ($Portable) { Say '  To remove it: Uninstall.cmd in its folder.' }
else { Say "  To remove it: ""Installed apps"" in Windows' settings, or Uninstall.cmd in its folder." }
Say '  If the picture stays black after the first screen, close the game and start it again (a rare fault at start-up, not yet found).'
