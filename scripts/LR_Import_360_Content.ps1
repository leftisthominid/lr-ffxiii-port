<#
  Lightning Returns ReXGlue port - import Xbox 360 saves (CON) and DLC (LIVE/PIRS).

  Reads each package (title 5351081B) and installs it where the ReXGlue build looks:

    Saves (content type 1, one APP.DAT):
      <UserData>\B13EBABEBABEBABE\5351081B\00000001\ff13-NN.dat\APP.DAT
      <UserData>\B13EBABEBABEBABE\5351081B\Headers\00000001\ff13-NN.dat.header
      NN comes from the file name if it is ff13-NN.dat, otherwise from the in-game name
      "Save Data MM" (NN = MM - 1).

    DLC (content type 2, e.g. "FFX-2 Yuna"):
      <UserData>\0000000000000000\5351081B\00000002\<package name>\<files>
      <UserData>\0000000000000000\5351081B\Headers\00000002\<package name>.header
      The header carries a full license mask, as a purchased package would.

  The source packages are only read. An existing slot/package is backed up first.

  Usage:
    powershell -ExecutionPolicy Bypass -File .\LR_Import_360_Content.ps1 -Source "D:\360content"
    powershell -ExecutionPolicy Bypass -File .\LR_Import_360_Content.ps1 -Source "C:\...\D10B46F3...053" -WhatIf
  -WhatIf only prints what would be installed.
#>
param(
  [Parameter(Mandatory = $true)][string]$Source,
  [string]$UserData = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'lightningreturns'),
  [switch]$WhatIf
)
$ErrorActionPreference = 'Stop'
$ProfileXuid = 'B13EBABEBABEBABE'
$TitleId = 0x5351081B

function U32BE([byte[]]$b, [int]$o) { ([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o + 1] -shl 16) -bor ([uint32]$b[$o + 2] -shl 8) -bor [uint32]$b[$o + 3] }
function U24LE([byte[]]$b, [int]$o) { [int]$b[$o] -bor ([int]$b[$o + 1] -shl 8) -bor ([int]$b[$o + 2] -shl 16) }
function U16LE([byte[]]$b, [int]$o) { [int]$b[$o] -bor ([int]$b[$o + 1] -shl 8) }
function S16BE([byte[]]$b, [int]$o) { $v = ([int]$b[$o] -shl 8) -bor [int]$b[$o + 1]; if ($v -ge 0x8000) { $v - 0x10000 } else { $v } }

# Reads an STFS package: content type, display name, and every file (path relative to the
# package root, bytes). Only files stored in consecutive blocks are supported, which is
# how saves and these DLC packages are written.
function Read-Package([string]$path) {
  $d = [IO.File]::ReadAllBytes($path)
  $magic = [Text.Encoding]::ASCII.GetString($d, 0, 4)
  if ($magic -notin 'CON ', 'LIVE', 'PIRS') { throw 'not an STFS package' }
  $headerSize = U32BE $d 0x340
  $contentType = U32BE $d 0x344
  $title = U32BE $d 0x360
  if ($title -ne $TitleId) { throw ('title {0:X8} is not Lightning Returns' -f $title) }
  if ($contentType -notin 1, 2) { throw ('content type {0:X8} is neither a save nor DLC' -f $contentType) }
  $display = [Text.Encoding]::BigEndianUnicode.GetString($d, 0x411, 0x80).Split([char]0)[0]
  $sex = (-bnot $d[0x37B]) -band 1
  $firstTable = ($headerSize + 0xFFF) -band 0xFFFFF000
  $backing = {
    param([int]$n)
    $r = ((([math]::Floor(($n + 0xAA) / 0xAA)) -shl $sex)) + $n
    if ($n -lt 0xAA) { return $r }
    if ($n -lt 0x70E4) { return $r + (([math]::Floor(($n + 0x70E4) / 0x70E4)) -shl $sex) }
    return (1 -shl $sex) + $r + (([math]::Floor(($n + 0x70E4) / 0x70E4)) -shl $sex)
  }
  $ftBlocks = U16LE $d (0x379 + 3)
  $ftBlock = U24LE $d (0x379 + 5)
  $entries = New-Object System.Collections.Generic.List[object]
  for ($blk = 0; $blk -lt [math]::Max(1, $ftBlocks); $blk++) {
    $ftOff = $firstTable + (& $backing ($ftBlock + $blk)) * 0x1000
    for ($i = 0; $i -lt 0x1000; $i += 0x40) {
      $e = $ftOff + $i
      $flags = $d[$e + 0x28]
      $len = $flags -band 0x3F
      if ($len -eq 0) { continue }
      $entries.Add([pscustomobject]@{
        Name = [Text.Encoding]::ASCII.GetString($d, $e, $len); IsDir = [bool]($flags -band 0x80)
        Consecutive = [bool]($flags -band 0x40); Blocks = U24LE $d ($e + 0x29); Start = U24LE $d ($e + 0x2F)
        Parent = S16BE $d ($e + 0x32); Size = [int](U32BE $d ($e + 0x34)) })
    }
  }
  $files = New-Object System.Collections.Generic.List[object]
  foreach ($en in $entries) {
    if ($en.IsDir) { continue }
    $rel = $en.Name; $p = $en.Parent; $guard = 0
    while ($p -ge 0 -and $p -lt $entries.Count -and $guard++ -lt 32) { $rel = $entries[$p].Name + '\' + $rel; $p = $entries[$p].Parent }
    if (-not $en.Consecutive -and $en.Blocks -gt 1) { throw "$rel is fragmented in this package; not supported" }
    $bytes = New-Object byte[] $en.Size
    $done = 0
    for ($k = 0; $k -lt $en.Blocks -and $done -lt $en.Size; $k++) {
      $o = $firstTable + (& $backing ($en.Start + $k)) * 0x1000
      $n = [math]::Min(0x1000, $en.Size - $done)
      [Array]::Copy($d, $o, $bytes, $done, $n)
      $done += $n
    }
    $files.Add([pscustomobject]@{ Path = $rel; Bytes = $bytes })
  }
  [pscustomobject]@{ ContentType = $contentType; Display = $display; Files = $files }
}

function New-Header([string]$fileName, [string]$display, [uint32]$contentType, [string]$xuidHex, [bool]$license) {
  $h = New-Object byte[] ($(if ($license) { 0x14C } else { 0x148 }))
  $h[3] = 1                                   # device_id = HDD (big-endian 1)
  $h[7] = [byte]$contentType                  # content_type
  $dn = [Text.Encoding]::BigEndianUnicode.GetBytes($display)
  [Array]::Copy($dn, 0, $h, 8, [math]::Min($dn.Length, 254))
  $fn = [Text.Encoding]::ASCII.GetBytes($fileName)
  [Array]::Copy($fn, 0, $h, 0x108, [math]::Min($fn.Length, 42))
  $x = [BitConverter]::GetBytes([uint64]::Parse($xuidHex, 'HexNumber')); [Array]::Reverse($x)
  [Array]::Copy($x, 0, $h, 0x138, 8)
  $t = [BitConverter]::GetBytes([uint32]$TitleId); [Array]::Reverse($t)
  [Array]::Copy($t, 0, $h, 0x140, 4)
  if ($license) { for ($i = 0x148; $i -lt 0x14C; $i++) { $h[$i] = 0xFF } }   # license mask: all bits
  return ,$h
}

$files = if (Test-Path -LiteralPath $Source -PathType Container) { Get-ChildItem -LiteralPath $Source -File -Recurse } else { Get-Item -LiteralPath $Source }
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
foreach ($f in $files) {
  try { $pkg = Read-Package $f.FullName } catch { Write-Host "  skip $($f.Name): $($_.Exception.Message)"; continue }
  if ($pkg.ContentType -eq 1) {
    $app = $pkg.Files | Where-Object { $_.Path -eq 'APP.DAT' } | Select-Object -First 1
    if (-not $app -or $app.Bytes.Length -ne 547616 -or $app.Bytes[0] -ne 0x0D -or
        [Text.Encoding]::ASCII.GetString($app.Bytes, 0x10, 19) -ne 'Total hours played:') {
      Write-Host "  skip $($f.Name): APP.DAT does not look like a Lightning Returns save"; continue
    }
    if ($f.Name -match '^ff13-(\d\d)\.dat$') { $slot = [int]$Matches[1] }
    elseif ($pkg.Display -match '(\d+)\s*$') { $slot = [int]$Matches[1] - 1 }
    else { Write-Host "  skip $($f.Name): cannot tell the slot from '$($pkg.Display)'"; continue }
    $name = 'ff13-{0:D2}.dat' -f $slot
    $xuid = $ProfileXuid
    $kind = 'save'
  } else {
    $name = $f.Name
    if ($name.Length -gt 42) { Write-Host "  skip $($f.Name): package file name longer than 42 characters"; continue }
    $xuid = '0000000000000000'
    $kind = 'DLC'
  }
  $typeDir = '{0:X8}' -f $pkg.ContentType
  $dir = Join-Path $UserData "$xuid\5351081B\$typeDir\$name"
  $headerRoot = Join-Path $UserData "$xuid\5351081B\Headers\$typeDir"
  Write-Host ("  {0} {1} -> {2}  ('{3}', {4} file(s))" -f $kind, $f.Name, $dir, $pkg.Display, $pkg.Files.Count)
  if ($WhatIf) { continue }
  if (Test-Path -LiteralPath $dir) {
    $bak = Join-Path $UserData "content-backup_$stamp\$typeDir\$name"
    New-Item -ItemType Directory -Force -Path (Split-Path $bak) | Out-Null
    Copy-Item -LiteralPath $dir -Destination $bak -Recurse
    Remove-Item -LiteralPath $dir -Recurse -Force
    Write-Host "     existing copy backed up to $bak"
  }
  New-Item -ItemType Directory -Force -Path $dir, $headerRoot | Out-Null
  foreach ($file in $pkg.Files) {
    $target = Join-Path $dir $file.Path
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    [IO.File]::WriteAllBytes($target, $file.Bytes)
  }
  [IO.File]::WriteAllBytes((Join-Path $headerRoot "$name.header"),
    (New-Header $name $pkg.Display $pkg.ContentType $xuid ($pkg.ContentType -eq 2)))
}
if ($WhatIf) { Write-Host '(-WhatIf: nothing was written)' } else { Write-Host 'Done.' }
