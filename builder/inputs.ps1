# Lightning Returns builder - finding and checking the game files the person owns. Every check
# only reads. Dot-sourced by builder.ps1 (uses its Say / Ask-YesNo).

$script:TitleId360 = 0x5351081B

function Read-Bytes([string]$path, [long]$offset, [int]$count) {
  $fs = [IO.File]::OpenRead($path)
  try {
    if ($offset + $count -gt $fs.Length) { return $null }
    $null = $fs.Seek($offset, 'Begin')
    $b = New-Object byte[] $count
    $read = 0
    while ($read -lt $count) {
      $n = $fs.Read($b, $read, $count - $read)
      if ($n -le 0) { return $null }
      $read += $n
    }
    return ,$b
  } finally { $fs.Dispose() }
}
function BE32([byte[]]$b, [int]$o) { return [uint32](([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o + 1] -shl 16) -bor ([uint32]$b[$o + 2] -shl 8) -bor $b[$o + 3]) }

# The title ID in a default.xex (the execution info header), or 0.
function Get-XexTitleId([string]$xex) {
  $h = Read-Bytes $xex 0 0x1000
  if (-not $h -or [Text.Encoding]::ASCII.GetString($h, 0, 4) -ne 'XEX2') { return 0 }
  $count = BE32 $h 0x14
  for ($i = 0; $i -lt $count -and 0x18 + $i * 8 + 8 -le $h.Length; ++$i) {
    if ((BE32 $h (0x18 + $i * 8)) -eq 0x00040006) {
      $at = BE32 $h (0x18 + $i * 8 + 4)
      $info = Read-Bytes $xex $at 0x18
      if ($info) { return BE32 $info 0x0C }
    }
  }
  return 0
}

# What a path holds, as far as the 360 game goes: @{ kind; path; why }.
#   kind 'folder' (extracted game: default.xex + weiss_data), 'iso' (disc image), 'god' (the
#   folder a 360 writes a Games on Demand / installed copy to), or '' with why.
function Test-Game360([string]$path) {
  $path = $path.Trim('"', ' ')
  if (-not (Test-Path -LiteralPath $path)) { return @{ kind = ''; why = "There is nothing at $path." } }
  $item = Get-Item -LiteralPath $path
  if ($item -is [IO.FileInfo]) {
    if ($item.Extension -ne '.iso') { return @{ kind = ''; why = 'That is a file, but not a disc image (.iso).' } }
    # XDVDFS ("MICROSOFT*XBOX*MEDIA") at sector 32 of the game partition: plain, XGD2, or XGD3.
    foreach ($base in 0, 0xFD90000, 0x2080000, 0x18300000) {
      $m = Read-Bytes $path ($base + 0x10000) 20
      if ($m -and [Text.Encoding]::ASCII.GetString($m) -eq 'MICROSOFT*XBOX*MEDIA') { return @{ kind = 'iso'; path = $item.FullName } }
    }
    return @{ kind = ''; why = 'That disc image is not an Xbox 360 game disc.' }
  }
  $xex = Join-Path $item.FullName 'default.xex'
  if (Test-Path -LiteralPath $xex) {
    $id = Get-XexTitleId $xex
    if ($id -ne $script:TitleId360) {
      return @{ kind = ''; why = ('This is not the Xbox 360 Lightning Returns (default.xex is for title {0:X8}, not 5351081B).' -f $id) }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $item.FullName 'weiss_data'))) {
      return @{ kind = ''; why = 'default.xex is here, but the weiss_data folder (the game data) is not.' }
    }
    return @{ kind = 'folder'; path = $item.FullName }
  }
  # A 360 USB drive / folder copied off a 360: Content\0000000000000000\5351081B\00007000.
  foreach ($sub in 'Content\0000000000000000\5351081B\00007000', '0000000000000000\5351081B\00007000', '5351081B\00007000', '00007000') {
    $god = Join-Path $item.FullName $sub
    if (Test-Path -LiteralPath $god) { return @{ kind = 'god'; path = $item.FullName } }
  }
  # 360 content for the game, but only its DLC (00000002): say so.
  $titles = @('Content\0000000000000000\5351081B', '0000000000000000\5351081B', '5351081B' | ForEach-Object { Join-Path $item.FullName $_ })
  if ($item.Name -eq '5351081B') { $titles += $item.FullName }
  foreach ($title in $titles) {
    if (Test-Path -LiteralPath (Join-Path $title '00000002')) {
      return @{ kind = ''; why = 'This holds Lightning Returns DLC copied off a 360 (00000002), not the game itself (00007000). Give the game disc image (.iso) or the extracted game folder here; the DLC is asked for later.' }
    }
  }
  return @{ kind = ''; why = 'No default.xex for 5351081B here, and no Lightning Returns copied off a 360 (Content\0000000000000000\5351081B\00007000).' }
}

function Find-Game360 {
  $found = @()
  foreach ($d in Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue) {
    foreach ($p in (Join-Path $d.Root 'Lightning Returns\sources\360\game'), $d.Root) {
      try { $t = Test-Game360 $p; if ($t.kind) { $found += $t } } catch { }
    }
  }
  return $found
}

function Ask-Path([string]$question, [scriptblock]$test, [object[]]$found, [switch]$Optional) {
  foreach ($f in $found) {
    if (Ask-YesNo ("Found {0} at {1}. Use it?" -f $f.label, $f.path)) { return $f }
  }
  while ($true) {
    $a = (Read-Host $question).Trim().Trim('"')
    if (-not $a) {
      if ($Optional) { return $null }
      Say 'This one is needed. Type or paste the path (drag the file or folder into this window works too).' 'Yellow'
      continue
    }
    $t = & $test $a
    if ($t.kind) { return $t }
    Say $t.why 'Yellow'
  }
}

function Ask-Game360 {
  $found = @(Find-Game360 | ForEach-Object { $_.label = 'the Xbox 360 game'; $_ })
  return Ask-Path 'Where is the Xbox 360 game? (disc image .iso, the extracted game folder, or the USB drive / folder from a 360)' ${function:Test-Game360} $found
}

# PS3 disc: an .iso (read directly, see iso.ps1) or a folder with PS3_GAME\PARAM.SFO.
function Test-Ps3([string]$path) {
  $path = $path.Trim('"', ' ')
  if (-not (Test-Path -LiteralPath $path)) { return @{ kind = ''; why = "There is nothing at $path." } }
  $item = Get-Item -LiteralPath $path
  $t = @{ kind = 'folder'; path = $item.FullName }
  if ($item -is [IO.FileInfo]) {
    if ($item.Extension -ne '.iso') { return @{ kind = ''; why = 'That is a file, but not a disc image (.iso).' } }
    $t.kind = 'iso'
    try {
      if (([LrIso]::Locate($item.FullName, 'PS3_GAME/PARAM.SFO'))[0] -eq -1) { return @{ kind = ''; why = 'This is not a PS3 disc image (no PS3_GAME\PARAM.SFO).' } }
    } catch { return @{ kind = ''; why = "Could not read the disc image: $($_.Exception.Message)" } }
  } elseif (-not (Test-Path -LiteralPath (Join-Path $item.FullName 'PS3_GAME\PARAM.SFO'))) {
    return @{ kind = ''; why = 'This is not a PS3 disc (no PS3_GAME\PARAM.SFO).' }
  }
  if ((Read-Ps3Sfo $t) -notmatch 'LIGHTNING RETURNS') { return @{ kind = ''; why = 'This PS3 disc is not Lightning Returns.' } }
  return $t
}

function Ask-Ps3([switch]$Required) {
  $found = @()
  foreach ($d in Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue) {
    foreach ($dir in (Join-Path $d.Root 'Lightning Returns'), (Join-Path $env:USERPROFILE 'Downloads')) {
      foreach ($iso in @(Get-ChildItem -LiteralPath $dir -Filter '*Lightning*Returns*.iso' -File -ErrorAction SilentlyContinue)) {
        $t = Test-Ps3 $iso.FullName
        if ($t.kind -and -not ($found | Where-Object { $_.path -eq $t.path })) { $t.label = 'the PS3 disc image'; $found += $t }
      }
    }
  }
  $q = 'Where is the PS3 disc image? (Enter to skip)'
  if ($Required) { $q = 'Where is the PS3 disc image?' }
  return Ask-Path $q ${function:Test-Ps3} $found -Optional:(-not $Required)
}

# Steam: the game folder (LRFF13.exe + weiss_data).
function Test-Steam([string]$path) {
  $path = $path.Trim('"', ' ')
  if (-not (Test-Path -LiteralPath $path -PathType Container)) { return @{ kind = ''; why = "There is no folder at $path." } }
  if (-not (Test-Path -LiteralPath (Join-Path $path 'weiss_data')) -or
      -not @(Get-ChildItem -LiteralPath $path -Filter 'LRFF13*.exe' -File -ErrorAction SilentlyContinue).Count) {
    return @{ kind = ''; why = 'This is not the Steam Lightning Returns folder (no LRFF13.exe and weiss_data).' }
  }
  return @{ kind = 'folder'; path = (Get-Item -LiteralPath $path).FullName }
}

function Find-Steam {
  $libs = @('C:\Program Files (x86)\Steam')
  $vdf = 'C:\Program Files (x86)\Steam\steamapps\libraryfolders.vdf'
  if (Test-Path -LiteralPath $vdf) {
    foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += $m.Groups[1].Value -replace '\\\\', '\' }
  }
  $found = @()
  foreach ($lib in $libs | Select-Object -Unique) {
    # A library on a drive that is not there now (unplugged) is skipped.
    try { $t = Test-Steam (Join-Path $lib 'steamapps\common\LIGHTNING RETURNS FINAL FANTASY XIII') } catch { continue }
    if ($t.kind) { $t.label = 'the Steam version'; $found += $t }
  }
  return $found
}

function Ask-Steam([switch]$Required) {
  $q = 'Where is the Steam version? (Enter to skip)'
  if ($Required) { $q = 'Where is the Steam version?' }
  return Ask-Path $q ${function:Test-Steam} @(Find-Steam) -Optional:(-not $Required)
}

# 360 DLC: the content folder off a 360 (...\5351081B\00000002) or loose packages. Each package
# must be an Xbox 360 package (LIVE / PIRS / CON) for title 5351081B.
function Test-Dlc360([string]$path) {
  $path = $path.Trim('"', ' ')
  if (-not (Test-Path -LiteralPath $path)) { return @{ kind = ''; why = "There is nothing at $path." } }
  $dir = $path
  foreach ($sub in 'Content\0000000000000000\5351081B\00000002', '0000000000000000\5351081B\00000002', '5351081B\00000002', '00000002') {
    if (Test-Path -LiteralPath (Join-Path $path $sub)) { $dir = Join-Path $path $sub; break }
  }
  $files = @(Get-ChildItem -LiteralPath $dir -File -Recurse -ErrorAction SilentlyContinue)
  if ((Get-Item -LiteralPath $path) -is [IO.FileInfo]) { $files = @(Get-Item -LiteralPath $path) }
  $packs = @()
  foreach ($f in $files) {
    $h = Read-Bytes $f.FullName 0 0x364
    if (-not $h) { continue }
    $magic = [Text.Encoding]::ASCII.GetString($h, 0, 4)
    if ($magic -in 'LIVE', 'PIRS', 'CON ' -and (BE32 $h 0x360) -eq $script:TitleId360) { $packs += $f.FullName }
  }
  if (-not $packs.Count) { return @{ kind = ''; why = 'No Lightning Returns 360 DLC packages here (Content\0000000000000000\5351081B\00000002 on the drive from the 360).' } }
  return @{ kind = 'packages'; path = (Get-Item -LiteralPath $dir).FullName; packages = $packs }
}

function Ask-Dlc360([switch]$Required) {
  $found = @()
  foreach ($d in Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue) {
    $t = $null
    try { if (Test-Path -LiteralPath (Join-Path $d.Root 'Content\0000000000000000\5351081B\00000002')) { $t = Test-Dlc360 $d.Root } } catch { }
    if ($t -and $t.kind) { $t.label = "the 360 DLC ($($t.packages.Count) package(s))"; $found += $t }
  }
  $q = 'Do you have the 360 DLC? (Enter to skip - you can add it any time later)'
  if ($Required) { $q = 'Where is the 360 DLC? (the drive or folder from the 360)' }
  return Ask-Path $q ${function:Test-Dlc360} $found -Optional:(-not $Required)
}

function Get-FreeGB([string]$path) {
  $root = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($path))
  $drive = Get-PSDrive -Name $root.Substring(0, 1) -ErrorAction SilentlyContinue
  if (-not $drive) { return 0 }
  return [math]::Floor($drive.Free / 1GB)
}

function Ask-Output($state) {
  $need = 20
  if ($state.target -eq 'both') { $need = 30 }
  # Default: "Lightning Returns" on the drive with the most free space.
  $best = Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | Where-Object { $_.Free } |
          Sort-Object Free -Descending | Select-Object -First 1
  $suggest = Join-Path $best.Root 'Lightning Returns'
  # Never a folder with things the builder did not make (an install set up by hand, for one).
  if ((Test-Path -LiteralPath $suggest) -and -not (Test-Path -LiteralPath (Join-Path $suggest 'builder_state.json')) -and
      @(Get-ChildItem -LiteralPath $suggest -Force -ErrorAction SilentlyContinue).Count) {
    $suggest = Join-Path $best.Root 'Lightning Returns Port'
  }
  while ($true) {
    $a = (Read-Host "Where should it go? (Enter for $suggest)").Trim().Trim('"')
    if (-not $a) { $a = $suggest }
    try { $full = [IO.Path]::GetFullPath($a) } catch { Say 'That is not a usable folder path.' 'Yellow'; continue }
    foreach ($src in $state.inputs.Values) {
      if ($src -and $src.path -and $full.StartsWith($src.path.TrimEnd('\') + '\', 'OrdinalIgnoreCase')) {
        Say 'Please pick a folder outside your game files (they are only read, never written).' 'Yellow'
        $full = $null
        break
      }
    }
    if (-not $full) { continue }
    if ((Test-Path -LiteralPath $full) -and -not (Test-Path -LiteralPath (Join-Path $full 'builder_state.json')) -and
        @(Get-ChildItem -LiteralPath $full -Force -ErrorAction SilentlyContinue).Count) {
      Say 'That folder already has files in it that the builder did not make. Please pick an empty or new folder.' 'Yellow'
      continue
    }
    $free = Get-FreeGB $full
    if ($free -lt $need) { Say "That drive has $free GB free; this needs about $need GB." 'Yellow'; continue }
    try {
      New-Item -ItemType Directory -Force -Path $full | Out-Null
      $probe = Join-Path $full '.builder_write_test'
      Set-Content -LiteralPath $probe -Value 'ok'
      Remove-Item -LiteralPath $probe -Force
    } catch { Say "Cannot write to $full ($($_.Exception.Message))." 'Yellow'; continue }
    return $full
  }
}

# ---- build state (builder_state.json in the output folder: choices + finished steps) ----
function New-BuildState([string]$target) {
  return [pscustomobject]@{ version = 1; target = $target; output = ''; inputs = @{}; shortcut = $false; done = @(); finished = $false }
}
function Read-BuildState([string]$output) {
  $f = Join-Path $output 'builder_state.json'
  if (-not (Test-Path -LiteralPath $f)) { return $null }
  $j = Get-Content -LiteralPath $f -Raw | ConvertFrom-Json
  $inputs = @{}
  foreach ($p in $j.inputs.PSObject.Properties) {
    if ($p.Value) { $h = @{}; foreach ($q in $p.Value.PSObject.Properties) { $h[$q.Name] = $q.Value }; $inputs[$p.Name] = $h }
  }
  return [pscustomobject]@{ version = $j.version; target = $j.target; output = $output; inputs = $inputs; shortcut = [bool]$j.shortcut
                            done = @($j.done); finished = [bool]$j.finished }
}
function Save-BuildState($state) {
  $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $state.output 'builder_state.json') -Encoding UTF8
  try { Set-Content -LiteralPath (Join-Path $Here 'last_output.txt') -Value $state.output } catch { }
}
# The output folder of the last build (remembered next to the builder), else a usual one.
function Find-ExistingOutput {
  $last = Join-Path $Here 'last_output.txt'
  if (Test-Path -LiteralPath $last) {
    $p = (Get-Content -LiteralPath $last -TotalCount 1).Trim()
    if ($p -and (Test-Path -LiteralPath (Join-Path $p 'builder_state.json'))) { return $p }
  }
  foreach ($d in Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue) {
    foreach ($name in 'Lightning Returns', 'Lightning Returns Port') {
      $p = Join-Path $d.Root $name
      if (Test-Path -LiteralPath (Join-Path $p 'builder_state.json')) { return $p }
    }
  }
  return ''
}
