# Lightning Returns builder - the build tools. Everything downloaded goes into <output>\tools,
# at a pinned version checked by SHA-256; nothing is installed system-wide, except Microsoft's
# C++ build tools (compiler libraries and Windows SDK), which only their installer can set up.
# Dot-sourced by pipeline.ps1.

$script:Downloads = [ordered]@{
  rexglue = @{ name = 'ReXGlue SDK 0.10.0'; size = 44751910
    url = 'https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/rexglue-sdk-0.10.0-win-amd64.zip'
    sha256 = 'AD4485DA853FBE932E9F5DBB643F9B5FE85DC73775EBBAD4DF6879FE6DF0C11B' }
  llvm = @{ name = 'LLVM / clang 23.1.3'; size = 901331280
    url = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.3/clang+llvm-23.1.3-x86_64-pc-windows-msvc.tar.xz'
    sha256 = '9EE6A2192FBB30C776415AF529927F76CA6B5D1D55699BB1315F9878E610B5B8' }
  cmake = @{ name = 'CMake 3.31.6'; size = 46473549
    url = 'https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.zip'
    sha256 = 'D163CD3AB4959B0A53FA8988F2DDBD2E6C501658201E6A154386BAD9DBE4F836' }
  ninja = @{ name = 'Ninja 1.12.1'; size = 275425
    url = 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip'
    sha256 = 'F550FEC705B6D6FF58F2DB3C374C2277A37691678D6ABA463ADCBB129108467A' }
  esrgan = @{ name = 'Real-ESRGAN (ncnn) 0.2.5.0'; size = 45474481
    url = 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip'
    sha256 = 'ABC02804E17982A3BE33675E4D471E91EA374E65B70167ABC09E31ACB412802D' }
  ffmpeg = @{ name = 'ffmpeg 7.1'; size = 92100272
    url = 'https://github.com/GyanD/codexffmpeg/releases/download/7.1/ffmpeg-7.1-essentials_build.zip'
    sha256 = 'FA7D4D7E795DB0E2503F49F105F46ED5852386F0CFDD819899BE3B65EBDE24FC' }
}

# A tool run with its output going to a log, so the builder can show that it is still working:
# the last line the tool printed (a build's "[n/N]" progress line when it has one).
if (-not ('LrRun' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

public class LrRun {
  readonly Process p = new Process();
  readonly StreamWriter log;
  readonly object gate = new object();
  string last = "", progress = "";
  static readonly Regex Ninja = new Regex(@"^\[\d+/\d+\]");

  // One argument as the C runtime / CommandLineToArgvW reads it back.
  public static string Quote(string a) {
    if (a.Length > 0 && a.IndexOfAny(new[] { ' ', '\t', '\n', '\v', '"' }) < 0) return a;
    var sb = new StringBuilder("\"");
    int slashes = 0;
    foreach (char c in a) {
      if (c == '\\') { slashes++; continue; }
      if (c == '"') sb.Append('\\', slashes * 2 + 1); else sb.Append('\\', slashes);
      slashes = 0;
      sb.Append(c);
    }
    sb.Append('\\', slashes * 2).Append('"');
    return sb.ToString();
  }

  public LrRun(string exe, string[] args, string logPath, string cwd) {
    log = new StreamWriter(logPath, false, new UTF8Encoding(false));
    var parts = new string[args.Length];
    for (int i = 0; i < args.Length; i++) parts[i] = Quote(args[i]);
    p.StartInfo.FileName = exe;
    p.StartInfo.Arguments = string.Join(" ", parts);
    p.StartInfo.UseShellExecute = false;
    p.StartInfo.CreateNoWindow = true;
    p.StartInfo.RedirectStandardOutput = true;
    p.StartInfo.RedirectStandardError = true;
    p.StartInfo.RedirectStandardInput = true;
    if (!string.IsNullOrEmpty(cwd)) p.StartInfo.WorkingDirectory = cwd;
    p.OutputDataReceived += (s, e) => Line(e.Data);
    p.ErrorDataReceived += (s, e) => Line(e.Data);
    try {
      p.Start();
    } catch {
      log.Dispose();
      throw;
    }
    p.StandardInput.Close();
    p.BeginOutputReadLine();
    p.BeginErrorReadLine();
  }

  void Line(string s) {
    if (s == null) return;
    lock (gate) {
      log.WriteLine(s);
      string t = s.Trim();
      if (t.Length == 0) return;
      last = t;
      if (Ninja.IsMatch(t)) progress = t;
    }
  }

  // What the tool is doing now: its last progress line, else its last line.
  public string Status {
    get { lock (gate) { return progress.Length > 0 ? progress : last; } }
  }

  public bool Wait(int ms) { return p.WaitForExit(ms); }

  public int Finish() {
    p.WaitForExit();  // also waits for the output to be read to the end
    lock (gate) { log.Dispose(); }
    return p.ExitCode;
  }
}
'@
}

# Shows "still working" lines while $done is false: every 15 s, the time so far and $status.
function Wait-WithProgress([scriptblock]$done, [scriptblock]$status) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (-not (& $done)) {
    $s = "$(& $status)"
    if ($s.Length -gt 110) { $s = $s.Substring(0, 107) + '...' }
    Say ("    ... {0:hh\:mm\:ss} {1}" -f $sw.Elapsed, $s) 'DarkGray'
  }
}

# Downloads $url to $dest, showing the MB received every 15 s.
function Invoke-Download([string]$url, [string]$dest, [long]$size = 0) {
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  $wc = New-Object Net.WebClient
  $task = $wc.DownloadFileTaskAsync($url, $dest)
  $total = $(if ($size) { ' of {0:N0} MB' -f ($size / 1MB) } else { '' })
  Wait-WithProgress { try { $task.Wait(15000) } catch { $true } } {
    $got = $(if (Test-Path -LiteralPath $dest) { (Get-Item -LiteralPath $dest).Length } else { 0 })
    '{0:N0} MB{1}' -f ($got / 1MB), $total
  }
  $wc.Dispose()
  if ($task.IsFaulted) { throw $task.Exception.GetBaseException() }
}

# Unzips $zip into $dest (in the background), showing that it is still working every 15 s.
function Expand-ZipWithProgress([string]$zip, [string]$dest) {
  $job = Start-Job -ScriptBlock {
    param($z, $d)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($z, $d)
  } -ArgumentList $zip, $dest
  try {
    Wait-WithProgress { [bool](Wait-Job $job -Timeout 15) } {
      $n = @(Get-ChildItem -LiteralPath $dest -Recurse -File -ErrorAction SilentlyContinue).Count
      "unpacking: $n files so far"
    }
    Receive-Job $job -ErrorAction Stop | Out-Null
    if ($job.State -ne 'Completed') { throw "Could not unpack $zip ($($job.ChildJobs[0].JobStateInfo.Reason))" }
  } finally { Remove-Job $job -Force }
}

# Downloads one pinned file into $dir (kept; a finished download is not fetched again).
function Get-Pinned([string]$key, [string]$dir) {
  $d = $script:Downloads[$key]
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $file = Join-Path $dir ([IO.Path]::GetFileName($d.url))
  if ((Test-Path -LiteralPath $file) -and (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -eq $d.sha256) { return $file }
  Say ("  downloading {0} ({1:N0} MB)..." -f $d.name, ($d.size / 1MB))
  $tmp = "$file.part"
  for ($try = 1; $try -le 3; ++$try) {
    try {
      Invoke-Download $d.url $tmp $d.size
      break
    } catch {
      if ($try -eq 3) { throw "Could not download $($d.name) from $($d.url): $($_.Exception.Message). Check the internet connection and run the builder again." }
      Say "  the download stopped ($($_.Exception.Message)); trying again" 'Yellow'
      Start-Sleep -Seconds (5 * $try)
    }
  }
  $hash = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash
  if ($hash -ne $d.sha256) {
    Remove-Item -LiteralPath $tmp -Force
    throw "The download of $($d.name) is not the expected file (SHA-256 $hash). Run the builder again; if it keeps happening, the download site changed the file."
  }
  Move-Item -LiteralPath $tmp -Destination $file -Force
  return $file
}

function Expand-Pinned([string]$key, [string]$downloads, [string]$dest, [string]$check) {
  if (Test-Path -LiteralPath (Join-Path $dest $check)) { return }
  $file = Get-Pinned $key $downloads
  Say "  unpacking $($script:Downloads[$key].name)..."
  $tmp = "$dest.unpack"
  if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $tmp | Out-Null
  if ($file.EndsWith('.zip')) {
    Expand-ZipWithProgress $file $tmp
  } else {
    # .tar.xz: Windows' own tar (Windows 10 1803 and later).
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    $rc = Invoke-Logged $tar @('-xf', $file, '-C', $tmp) "$tmp.log"
    if ($rc -ne 0) { throw "Could not unpack $file (Windows tar.exe failed; see $tmp.log)." }
  }
  # Archives that hold one top folder: use its content.
  $items = @(Get-ChildItem -LiteralPath $tmp)
  $from = $tmp
  if ($items.Count -eq 1 -and $items[0].PSIsContainer) { $from = $items[0].FullName }
  if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
  Move-Item -LiteralPath $from -Destination $dest
  if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
  if (-not (Test-Path -LiteralPath (Join-Path $dest $check))) { throw "$($script:Downloads[$key].name) unpacked, but $check is not in it." }
}

# Microsoft's C++ build tools (MSVC libraries + Windows SDK), which clang uses on Windows.
function Find-MsvcTools {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
  $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $path) { return $null }
  $sdk = Get-ChildItem -LiteralPath (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include') -Directory -ErrorAction SilentlyContinue
  if (-not $sdk) { return $null }
  return $path
}

function Install-MsvcTools {
  Say ''
  Say 'The C++ build tools from Microsoft (Visual Studio 2022 Build Tools with "Desktop development'
  Say 'with C++") are needed to build the game and its converters, and they are not on this PC. Only'
  Say 'their own installer can set them up (about 6 GB; Windows asks for permission once).'
  if (-not (Ask-YesNo 'Install them now?')) {
    throw 'The Microsoft C++ build tools are needed. Install "Visual Studio 2022 Build Tools" with "Desktop development with C++", then run the builder again.'
  }
  $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
  if ($winget) {
    & $winget.Source install --id Microsoft.VisualStudio.2022.BuildTools --exact --accept-package-agreements --accept-source-agreements `
      --override '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
  } else {
    $boot = Join-Path $env:TEMP 'vs_BuildTools.exe'
    (New-Object Net.WebClient).DownloadFile('https://aka.ms/vs/17/release/vs_BuildTools.exe', $boot)
    $p = Start-Process -FilePath $boot -Wait -PassThru -ArgumentList @('--quiet', '--wait', '--norestart',
      '--add', 'Microsoft.VisualStudio.Workload.VCTools', '--includeRecommended')
    if ($p.ExitCode -notin 0, 3010) { throw "The Build Tools installer stopped with code $($p.ExitCode)." }
  }
  if (-not (Find-MsvcTools)) { throw 'The Microsoft C++ build tools still are not found after the installer ran. Restart the PC and run the builder again.' }
}

# clang 20 or newer (ReXGlue 0.10 needs it): an installed LLVM, else the pinned archive, else
# LLVM's winget package. Returns its bin folder.
function Get-ClangMajor([string]$clangxx) {
  try {
    $v = & $clangxx --version 2>$null | Select-Object -First 1
    if ($v -match 'clang version (\d+)\.') { return [int]$Matches[1] }
  } catch { }
  return 0
}
function Find-Llvm([string]$toolsDir, [string]$dl) {
  $mine = Join-Path $toolsDir 'llvm\bin'
  foreach ($bin in $mine, (Join-Path $env:ProgramFiles 'LLVM\bin')) {
    if ((Get-ClangMajor (Join-Path $bin 'clang++.exe')) -ge 20) { return $bin }
  }
  try {
    Expand-Pinned 'llvm' $dl (Join-Path $toolsDir 'llvm') 'bin\clang++.exe'
    if ((Get-ClangMajor (Join-Path $mine 'clang++.exe')) -ge 20) { return $mine }
  } catch {
    Say "  $($_.Exception.Message)" 'Yellow'
  }
  Say 'LLVM (clang 20 or newer) could not be unpacked here; installing it with winget instead (Windows asks for permission once).'
  $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
  if (-not $winget) { throw 'Install LLVM 20 or newer from https://github.com/llvm/llvm-project/releases (the win64 installer), then run the builder again.' }
  & $winget.Source install --id LLVM.LLVM --exact --silent --accept-package-agreements --accept-source-agreements
  $bin = Join-Path $env:ProgramFiles 'LLVM\bin'
  if ((Get-ClangMajor (Join-Path $bin 'clang++.exe')) -ge 20) { return $bin }
  throw 'LLVM was not found after installing it. Install LLVM 20 or newer, then run the builder again.'
}

# All the Windows build tools -> @{ clang; clangxx; rc; lld; ar; cmake; ninja; sdk; sdkBin }.
function Get-WindowsTools([string]$toolsDir) {
  if (-not (Find-MsvcTools)) { Install-MsvcTools }
  $dl = Join-Path $toolsDir 'downloads'
  $llvm = Find-Llvm $toolsDir $dl
  Expand-Pinned 'cmake' $dl (Join-Path $toolsDir 'cmake') 'bin\cmake.exe'
  Expand-Pinned 'ninja' $dl (Join-Path $toolsDir 'ninja') 'ninja.exe'
  Expand-Pinned 'rexglue' $dl (Join-Path $toolsDir 'rexglue-sdk') 'bin\rexglue.exe'
  $sdk = Join-Path $toolsDir 'rexglue-sdk'
  # The release zip holds win-amd64\ at the top on some versions.
  if (Test-Path -LiteralPath (Join-Path $sdk 'win-amd64\bin\rexglue.exe')) { $sdk = Join-Path $sdk 'win-amd64' }
  $t = @{
    clang = Join-Path $llvm 'clang.exe'; clangxx = Join-Path $llvm 'clang++.exe'; rc = Join-Path $llvm 'llvm-rc.exe'
    lld = Join-Path $llvm 'lld-link.exe'; ar = Join-Path $llvm 'llvm-ar.exe'; llvmBin = $llvm
    cmake = Join-Path $toolsDir 'cmake\bin\cmake.exe'; ninja = Join-Path $toolsDir 'ninja\ninja.exe'
    sdk = $sdk; sdkBin = Join-Path $sdk 'bin'; rexglue = Join-Path $sdk 'bin\rexglue.exe'
  }
  foreach ($k in 'clang', 'clangxx', 'rc', 'lld', 'ar', 'cmake', 'ninja', 'rexglue') {
    if (-not (Test-Path -LiteralPath $t[$k])) { throw "Build tool missing after setup: $($t[$k])" }
  }
  $env:PATH = "$llvm;$env:PATH"
  return $t
}

function Get-Ffmpeg([string]$toolsDir) {
  Expand-Pinned 'ffmpeg' (Join-Path $toolsDir 'downloads') (Join-Path $toolsDir 'ffmpeg') 'bin\ffmpeg.exe'
  return Join-Path $toolsDir 'ffmpeg\bin\ffmpeg.exe'
}

# The CMake arguments every build here uses (clang targeting MSVC, our Ninja, the SDK).
function Get-CMakeToolArgs($t) {
  return @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_C_COMPILER=$($t.clang -replace '\\','/')", "-DCMAKE_CXX_COMPILER=$($t.clangxx -replace '\\','/')",
    "-DCMAKE_MAKE_PROGRAM=$($t.ninja -replace '\\','/')", "-DCMAKE_PREFIX_PATH=$($t.sdk -replace '\\','/')",
    "-DCMAKE_RC_COMPILER=$($t.rc -replace '\\','/')", "-DCMAKE_LINKER=$($t.lld -replace '\\','/')",
    "-DCMAKE_AR=$($t.ar -replace '\\','/')")
}
