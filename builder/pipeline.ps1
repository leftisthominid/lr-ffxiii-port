# Lightning Returns builder - the build steps. Each finished step is recorded in
# builder_state.json, so a stopped build carries on from the next one. The person's game files
# are only read; everything is written under the output folder (and the game's user data folder,
# Documents\lightningreturns, for DLC). Dot-sourced by builder.ps1.

. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'patches.ps1')
. (Join-Path $PSScriptRoot 'android.ps1')

$script:XexSha256 = '52D3C11378FA041E086DABFD79994E7EAA63E47F28E5C98878C71573823D5AFB'
$script:Repo = Split-Path $PSScriptRoot  # the builder folder (project\, plugin\, converters\, scripts\)

function Get-Paths($state) {
  $o = $state.output
  $docs = [Environment]::GetFolderPath('MyDocuments')
  if (-not $docs) { $docs = Join-Path $HOME 'Documents' }  # Linux (testing)
  $game = Join-Path $o 'sources\360\game'
  if ($state.inputs.game360 -and $state.inputs.game360.kind -eq 'folder') { $game = $state.inputs.game360.path }
  return @{
    out = $o; tools = Join-Path $o 'tools'; logs = Join-Path $o 'logs'; work = Join-Path $o 'work'
    game = $game
    project = Join-Path $o 'native\LightningReturns'
    build = Join-Path $o 'native\LightningReturns\out\build\win-amd64-release'
    plugin = Join-Path $o 'native\rexgpu-plugin'
    pluginBuild = Join-Path $o 'native\rexgpu-plugin-build'
    install = Join-Path $o 'Lightning Returns'          # the game: lightningreturns.exe and what it uses
    textures = Join-Path $o 'Lightning Returns\textures'
    icon = Join-Path $o 'icon'
    userData = Join-Path $docs 'lightningreturns'
  }
}

# Runs a tool, its output into a log; returns the exit code. While it runs, a line every 15 s
# shows that it is still working; the last lines of the log are shown on failure.
function Invoke-Logged([string]$exe, [string[]]$arguments, [string]$log, [string]$cwd = '', [switch]$Quiet) {
  New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
  try {
    $run = New-Object LrRun $exe, ([string[]]@($arguments)), $log, $cwd
  } catch {
    $e = $_.Exception
    while ($e.InnerException) { $e = $e.InnerException }
    Set-Content -LiteralPath $log -Value "Could not start $exe : $($e.Message)"
    Say "  could not start $exe ($($e.Message))" 'Yellow'
    return 9009
  }
  Wait-WithProgress { $run.Wait(15000) } { $run.Status }
  $rc = $run.Finish()
  if ($rc -ne 0 -and -not $Quiet) { Show-LogError $log }
  return $rc
}

# The part of a failed tool's log that says why: a build's first "FAILED:" step and the compiler
# errors under it (ninja keeps going with other jobs, so the end of the log is not the error),
# else the last lines.
function Show-LogError([string]$log) {
  $lines = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue)
  $at = -1
  for ($i = 0; $i -lt $lines.Count; ++$i) { if ($lines[$i] -match '^FAILED: ') { $at = $i; break } }
  if ($at -ge 0) {
    Say "  (the failed step in $log)" 'Yellow'
    $shown = 0
    for ($i = $at; $i -lt $lines.Count -and $shown -lt 25; ++$i) {
      if ($i -gt $at -and $lines[$i] -match '^\[\d+/\d+\] ') { break }  # the next build step
      $t = $lines[$i]
      if ($t.Length -gt 300) { $t = $t.Substring(0, 297) + '...' }
      Say "    $t" 'Yellow'
      $shown++
    }
  } else {
    Say "  (last lines of $log)" 'Yellow'
    $lines | Select-Object -Last 15 | ForEach-Object { Say "    $_" 'Yellow' }
  }
}

function Build-Converter([hashtable]$t, [string]$source, [string]$exe, [string[]]$extra = @()) {
  if ((Test-Path -LiteralPath $exe) -and (Get-Item -LiteralPath $exe).LastWriteTime -gt (Get-Item -LiteralPath $source).LastWriteTime) { return }
  $rc = Invoke-Logged $t.clangxx (@('-O2', '-std=c++17') + $extra + @($source, '-o', $exe)) "$exe.build.log"
  if ($rc -ne 0 -or -not (Test-Path -LiteralPath $exe)) { throw "Could not build $(Split-Path $exe -Leaf); see $exe.build.log" }
}

# ---------------------------------------------------------------------------------------------
# Steps. Each takes the state and the paths; throws with a plain reason on failure.

function Step-Tools($state, $p) {
  $script:T = Get-WindowsTools $p.tools
  Say "  clang: $($script:T.clangxx)"
}

function Step-Game($state, $p) {
  $g = $state.inputs.game360
  if ($g.kind -ne 'folder') {
    # Disc image or a 360's USB copy: unpacked once into sources\360\game.
    $exe = Join-Path $p.work 'lr_extract360.exe'
    Build-Converter $script:T (Join-Path $script:Repo 'converters\setup\lr_extract360.cpp') $exe
    $tmp = "$($p.game).new"
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
    Say '  unpacking the 360 game (about 7 GB, 5-15 minutes)...'
    $rc = Invoke-Logged $exe @($g.path, $tmp) (Join-Path $p.logs 'extract360.log')
    if ($rc -ne 0) { throw "Could not unpack the 360 game from $($g.path); see $($p.logs)\extract360.log" }
    if (Test-Path -LiteralPath $p.game) { Remove-Item -LiteralPath $p.game -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Split-Path $p.game) | Out-Null
    Move-Item -LiteralPath $tmp -Destination $p.game
  }
  # The 360 release's main files (one disc / Games on Demand package).
  $expected = [ordered]@{
    'weiss_data\movie\movieu.x360.bin' = 709599232; 'weiss_data\movie\movie2u.x360.bin' = 612515840
    'weiss_data\movie\movie3u.x360.bin' = 679231488; 'weiss_data\sys\white_imgu.x360.bin' = 503519232
    'weiss_data\sys\white_img2u.x360.bin' = 583780352; 'weiss_data\sys\white_scru.x360.bin' = 11626496
    'weiss_data\zone\white_z0100u_img.x360.bin' = 1778321408; 'weiss_data\zone\white_z0110u_img.x360.bin' = 40071168
    'weiss_data\zone\white_z0120u_img.x360.bin' = 1038596096; 'weiss_data\zone\white_z0121u_img.x360.bin' = 816568320
  }
  foreach ($k in $expected.Keys) {
    $f = Join-Path $p.game $k
    if (-not (Test-Path -LiteralPath $f)) { throw "The 360 game files are incomplete: $k is missing." }
    if ((Get-Item -LiteralPath $f).Length -ne $expected[$k]) { throw "The 360 game files do not match the release the port is made for: $k has a different size." }
  }
  $xex = Join-Path $p.game 'default.xex'
  $hash = (Get-FileHash -LiteralPath $xex -Algorithm SHA256).Hash
  if ($hash -ne $script:XexSha256) {
    throw ("This default.xex is a different version of the game than the one the port is made for (SHA-256 {0}). " +
           "The port needs the Xbox 360 release as it is on the disc / Games on Demand, without a title update.") -f $hash
  }
  Say "  $($p.game) (default.xex checked)"
}

# The icon (exe and Android app): Lightning's face from the title screen's background in the
# player's own 360 game data, upscaled 4x with Real-ESRGAN (on the graphics card; a plain
# enlargement when that cannot run), composed by lr_icon. Made here, never part of the repository.
function Step-Icon($state, $p) {
  $exe = Join-Path $p.work 'lr_icon.exe'
  Build-Converter $script:T (Join-Path $script:Repo 'converters\icon\lr_icon.cpp') $exe
  New-Item -ItemType Directory -Force -Path $p.icon | Out-Null
  $tile = Join-Path $p.work 'title_face.png'
  $rc = Invoke-Logged $exe @('title', $p.game, $tile) (Join-Path $p.logs 'icon.log')
  if ($rc -ne 0) {
    # Only the icon: the build goes on, the exe and the app get a plain icon.
    Say "  could not read the title screen picture (see $(Join-Path $p.logs 'icon.log')); the game gets a plain icon" 'Yellow'
    return
  }
  $big = Join-Path $p.work 'title_face_x4.png'
  try {
    Expand-Pinned 'esrgan' (Join-Path $p.tools 'downloads') (Join-Path $p.tools 'realesrgan') 'realesrgan-ncnn-vulkan.exe'
    $esr = Join-Path $p.tools 'realesrgan\realesrgan-ncnn-vulkan.exe'
    $rc = Invoke-Logged $esr @('-i', $tile, '-o', $big, '-n', 'realesrgan-x4plus', '-m', (Join-Path $p.tools 'realesrgan\models')) (Join-Path $p.logs 'icon_upscale.log')
    if ($rc -ne 0 -or -not (Test-Path -LiteralPath $big)) { throw 'the upscaler did not run' }
  } catch {
    Say "  the AI upscale did not work ($($_.Exception.Message)); the icon is enlarged plainly instead" 'Yellow'
    $big = $tile
  }
  $rc = Invoke-Logged $exe @('compose', $big, $p.icon) (Join-Path $p.logs 'icon.log')
  if ($rc -ne 0) {
    Say "  could not make the icon (see $(Join-Path $p.logs 'icon.log')); the game gets a plain icon" 'Yellow'
    if (Test-Path -LiteralPath $p.icon) { Remove-Item -LiteralPath $p.icon -Recurse -Force }
    return
  }
  Say "  $($p.icon)"
}

function Step-Project($state, $p) {
  $src = Join-Path $script:Repo 'project'
  New-Item -ItemType Directory -Force -Path $p.project | Out-Null
  foreach ($d in 'src', 'analysis', 'generated') { New-Item -ItemType Directory -Force -Path (Join-Path $p.project $d) | Out-Null }
  Copy-Item -Path (Join-Path $src 'src\*') -Destination (Join-Path $p.project 'src') -Force
  Copy-Item -Path (Join-Path $src 'analysis\*') -Destination (Join-Path $p.project 'analysis') -Force
  Copy-Item -LiteralPath (Join-Path $src 'generated\rexglue.cmake') -Destination (Join-Path $p.project 'generated') -Force
  foreach ($f in 'CMakeLists.txt', 'CMakePresets.json', 'lr_guest_irq.inc') { Copy-Item -LiteralPath (Join-Path $src $f) -Destination $p.project -Force }
  # The icon as a Windows resource (CMakeLists.txt adds the .rc when it is there).
  $ico = Join-Path $p.icon 'lightningreturns.ico'
  if (Test-Path -LiteralPath $ico) {
    Copy-Item -LiteralPath $ico -Destination (Join-Path $p.project 'src') -Force
    [IO.File]::WriteAllText((Join-Path $p.project 'src\lightningreturns.rc'), "1 ICON `"lightningreturns.ico`"`r`n")
  }
  $manifest = [IO.File]::ReadAllText((Join-Path $src 'lightningreturns_manifest.toml.in')).Replace('@GAME_ROOT@', ($p.game -replace '\\', '/'))
  [IO.File]::WriteAllText((Join-Path $p.project 'lightningreturns_manifest.toml'), $manifest)
  Say "  $($p.project)"
}

function Step-Codegen($state, $p) {
  Say '  recompiling the game code from default.xex (5-10 minutes)...'
  $gen = Join-Path $p.project 'generated\default'
  $rc = Invoke-Logged $script:T.rexglue @('codegen', 'lightningreturns_manifest.toml') (Join-Path $p.logs 'codegen.log') $p.project
  if ($rc -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $gen 'lightningreturns_pch.h'))) { throw "The recompiler failed; see $($p.logs)\codegen.log" }
  Say ('  ' + (Update-PchIrqLock (Join-Path $gen 'lightningreturns_pch.h') (Join-Path $p.project 'lr_guest_irq.inc')))
  Say ('  ' + (Add-MemoryBarriers $gen))
}

function Step-Exe($state, $p) {
  # lr_gameinput.h back in place if an earlier failed run took it out.
  $giRepo = Join-Path $script:Repo 'project\src\lr_gameinput.h'
  $giProject = Join-Path $p.project 'src\lr_gameinput.h'
  if ((Test-Path -LiteralPath $giRepo) -and -not (Test-Path -LiteralPath $giProject)) { Copy-Item -LiteralPath $giRepo -Destination $giProject }
  $cfg = @('-S', $p.project, '-B', $p.build) + (Get-CMakeToolArgs $script:T) + @('-DCMAKE_EXE_LINKER_FLAGS=-Xlinker /MAP:lightningreturns.map')
  $rc = Invoke-Logged $script:T.cmake $cfg (Join-Path $p.logs 'configure.log')
  if ($rc -ne 0) { $rc = Invoke-Logged $script:T.cmake $cfg (Join-Path $p.logs 'configure2.log') }
  if ($rc -ne 0) { throw "Configuring the game build failed; see $($p.logs)\configure2.log" }
  Say '  building the game (all recompiled code: 15-45 minutes)...'
  $jobs = [Environment]::ProcessorCount
  $rc = Invoke-Logged $script:T.cmake @('--build', $p.build, '--target', 'lightningreturns', '--parallel', "$jobs") (Join-Path $p.logs 'build_exe.log')
  $gi = Join-Path $p.project 'src\lr_gameinput.h'
  if ($rc -ne 0 -and (Test-Path -LiteralPath $gi)) {
    Say '  building again without the GameInput part (the Share button then works where SDL reports it)' 'Yellow'
    Remove-Item -LiteralPath $gi -Force
    $rc = Invoke-Logged $script:T.cmake @('--build', $p.build, '--target', 'lightningreturns', '--parallel', "$jobs") (Join-Path $p.logs 'build_exe2.log')
  }
  if ($rc -ne 0) { throw "The game build failed; see $($p.logs)\build_exe*.log" }
  foreach ($dll in 'rexruntime.dll', 'rexgpu-xenos.dll') {
    Copy-Item -LiteralPath (Join-Path $script:T.sdkBin $dll) -Destination $p.build -Force
  }
  Say "  $($p.build)\lightningreturns.exe"
}

function Step-Plugin($state, $p) {
  if (Test-Path -LiteralPath $p.plugin) { Remove-Item -LiteralPath $p.plugin -Recurse -Force }
  Copy-Item -LiteralPath (Join-Path $script:Repo 'plugin') -Destination $p.plugin -Recurse
  $cfg = @('-S', $p.plugin, '-B', $p.pluginBuild) + (Get-CMakeToolArgs $script:T)
  $rc = Invoke-Logged $script:T.cmake $cfg (Join-Path $p.logs 'plugin_configure.log')
  if ($rc -ne 0) { $rc = Invoke-Logged $script:T.cmake $cfg (Join-Path $p.logs 'plugin_configure2.log') }
  if ($rc -ne 0) { throw "Configuring the renderer build failed; see $($p.logs)\plugin_configure2.log" }
  Say '  building the renderer (5-15 minutes)...'
  $rc = Invoke-Logged $script:T.cmake @('--build', $p.pluginBuild, '--parallel', "$([Environment]::ProcessorCount)") (Join-Path $p.logs 'build_plugin.log')
  if ($rc -ne 0) { throw "The renderer build failed; see $($p.logs)\build_plugin.log" }
  $dll = Get-ChildItem -LiteralPath $p.pluginBuild -Recurse -Filter 'rexgpu-xenos.dll' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $dll) { throw "The renderer build produced no rexgpu-xenos.dll; see $($p.logs)\build_plugin.log" }
  Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $p.build 'rexgpu-xenos.dll') -Force
  Say '  rexgpu-xenos.dll installed next to the game'
}

# The game folder the player opens: lightningreturns.exe with what it needs next to it, started
# by double-clicking (no launcher). lightningreturns.toml holds the options a launcher used to
# pass; lightningreturns_launch.txt points at the 360 game files.
function Step-Install($state, $p) {
  New-Item -ItemType Directory -Force -Path $p.install | Out-Null
  foreach ($f in 'lightningreturns.exe', 'rexruntime.dll', 'rexgpu-xenos.dll') {
    Copy-Item -LiteralPath (Join-Path $p.build $f) -Destination $p.install -Force
  }
  $launchFile = Join-Path $p.install 'lightningreturns_launch.txt'
  $launch = @('# Written by the builder: where the Xbox 360 game files are (only read).', "game_data_root = $($p.game)")
  # LR_ settings already in the file (added by later steps or by hand, e.g. LR_TEXTURE_USER) stay.
  if (Test-Path -LiteralPath $launchFile) { $launch += @(Get-Content -LiteralPath $launchFile | Where-Object { $_ -match '^\s*LR_' }) }
  [IO.File]::WriteAllLines($launchFile, [string[]]$launch)
  Say "  $($p.install)\lightningreturns.exe"
  $lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Lightning Returns.lnk'
  if ($state.shortcut -and -not (Test-Path -LiteralPath $lnk)) {
    $shell = New-Object -ComObject WScript.Shell
    $s = $shell.CreateShortcut($lnk)
    $s.TargetPath = Join-Path $p.install 'lightningreturns.exe'
    $s.WorkingDirectory = $p.install
    $s.Save()
    Say "  $lnk"
  }
}

function Step-Settings($state, $p) {
  # Renderer settings the port is tuned for, and the options a launcher used to pass.
  New-Item -ItemType Directory -Force -Path $p.install | Out-Null
  $toml = Join-Path $p.install 'lightningreturns.toml'
  $keep = @()
  if (Test-Path -LiteralPath $toml) {
    $keep = @(Get-Content -LiteralPath $toml | Where-Object { $_ -notmatch '^\s*(gpu_plugin|graphics_backend|log_level|anisotropic_override|texture_cache_memory_limit_soft|texture_cache_memory_limit_hard|clear_memory_page_state|occlusion_query_enable|readback_memexport)\s*=' })
  }
  # Resolution: the game's Settings > Resolution (Auto follows the display).
  $lines = @('gpu_plugin = "xenos"', 'graphics_backend = "d3d12"', 'log_level = "info"', 'anisotropic_override = 5',
             'texture_cache_memory_limit_soft = 2048', 'texture_cache_memory_limit_hard = 4096',
             'clear_memory_page_state = false', 'occlusion_query_enable = false', 'readback_memexport = false')
  [IO.File]::WriteAllLines($toml, [string[]]($lines + $keep))
  # The runtime looks for saves under 00000000BABEBABE when listing them (LOAD GAME); they are
  # written under B13EBABEBABEBABE. A junction makes both the same folder.
  $real = Join-Path $p.userData 'B13EBABEBABEBABE'
  $alias = Join-Path $p.userData '00000000BABEBABE'
  New-Item -ItemType Directory -Force -Path $real | Out-Null
  $item = Get-Item -LiteralPath $alias -Force -ErrorAction SilentlyContinue
  if (-not $item) {
    New-Item -ItemType Junction -Path $alias -Target $real | Out-Null
    Say "  save folder link: $alias -> $real"
  } elseif ($item.LinkType -ne 'Junction') {
    Say "  $alias is a normal folder, not a link; LOAD GAME may not list saves until it is removed" 'Yellow'
  }
}

# Sharp fonts: the game's bitmap font atlases, number sheets, and loading lettering rebuilt at 4x
# from the 360 game's own glyphs (lr_uiconv --fonts), in the game folder's textures\replace\fonts,
# which the renderer always uses. Built next to the old set and swapped in when complete.
function Step-Fonts($state, $p) {
  $exe = Join-Path $p.work 'lr_uiconv.exe'
  $xx = Get-ChildItem -LiteralPath (Join-Path $script:T.sdk 'include') -Recurse -Filter 'xxhash.h' | Select-Object -First 1
  Build-Converter $script:T (Join-Path $script:Repo 'converters\uiconv\lr_uiconv.cpp') $exe @("-I$($xx.DirectoryName)")
  $out = Join-Path $p.textures 'replace\fonts'
  $new = "$out.new"
  if (Test-Path -LiteralPath $new) { Remove-Item -LiteralPath $new -Recurse -Force }
  New-Item -ItemType Directory -Force -Path (Split-Path $new) | Out-Null
  Say '  rebuilding the fonts at 4x (about a minute)...'
  $rc = Invoke-Logged $exe @('--fonts', (Join-Path $script:Repo 'converters\uiconv\ui_sheets.csv'), (Join-Path $p.game 'weiss_data\sys'), $new,
    '--scale', '4', '--sheet', 'gui/resident/pack/pack57', '--sheet', 'gui/resident/pack/pack58') (Join-Path $p.logs 'fonts.log')
  if ($rc -ne 0) {
    Remove-Item -LiteralPath $new -Recurse -Force -ErrorAction SilentlyContinue
    throw "Rebuilding the fonts failed; see $(Join-Path $p.logs 'fonts.log')"
  }
  if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
  Move-Item -LiteralPath $new -Destination $out
  Say ("  {0} font textures in {1}" -f @(Get-ChildItem -LiteralPath $out -Filter '*.dds').Count, $out)
}

function Step-Ps3Ui($state, $p) {
  $ps3 = $state.inputs.ps3
  $sys = Join-Path $p.work 'ps3\sys'
  $sizes = @{ 'white_imgu' = 477366272; 'white_img2u' = 562712576 }
  foreach ($k in $sizes.Keys) {
    $f = Join-Path $sys "$k.ps3.bin"
    if ((Test-Path -LiteralPath $f) -and (Get-Item -LiteralPath $f).Length -eq $sizes[$k]) { continue }
    Say "  reading $k.ps3.bin from the PS3 disc..."
    Copy-Ps3File $ps3 "PS3_GAME/USRDIR/weiss_data/sys/$k.ps3.bin" $f
    if ((Get-Item -LiteralPath $f).Length -ne $sizes[$k]) {
      throw "$k.ps3.bin on this PS3 disc is not the expected size: only the USA disc (BLUS31311, En/Fr/Es) is supported for the PS3 buttons so far."
    }
  }
  $exe = Join-Path $p.work 'lr_uiconv.exe'
  $xx = Get-ChildItem -LiteralPath (Join-Path $script:T.sdk 'include') -Recurse -Filter 'xxhash.h' | Select-Object -First 1
  Build-Converter $script:T (Join-Path $script:Repo 'converters\uiconv\lr_uiconv.cpp') $exe @("-I$($xx.DirectoryName)")
  $out = Join-Path $p.textures 'ps3ui'
  if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
  $rc = Invoke-Logged $exe @('--table', (Join-Path $script:Repo 'converters\uiconv\ui_sheets.csv'), (Join-Path $p.game 'weiss_data\sys'), $sys, $out) (Join-Path $p.logs 'ps3ui.log')
  if ($rc -ne 0) { throw "Converting the PS3 buttons / UI failed; see $($p.logs)\ps3ui.log" }
  Remove-Item -LiteralPath $sys -Recurse -Force  # 1 GB, only needed for the conversion
  Say ("  {0} PS3 UI textures in {1}" -f @(Get-ChildItem -LiteralPath $out -Filter '*.dds').Count, $out)
}

# The PS3 cinematics: the video of 21 movies, copied (no re-encoding) out of the PS3 movie packs.
# name, pack, position in the pack (2048-byte units), size - from movielist*u.ps3.bin.
$script:Ps3Movies = @(
  @('GEN_us', 'movieu', 0x0, 0x191d5000), @('OPA_us', 'movieu', 0x323aa, 0x4a811800),
  @('tm_lxaa_010_us', 'movieu', 0xc73cd, 0x17e7000), @('tm_lxaa_160_us', 'movieu', 0xca39b, 0x4496000),
  @('tm_lxaa_090_us', 'movieu', 0xd2cc7, 0x3164800), @('hm_lxaa_150_us', 'movieu', 0xd8f90, 0x196a800),
  @('hm_lxaa_190_us', 'movieu', 0xdc265, 0x5617000),
  @('ENB_us', 'movie2u', 0x0, 0x40500800), @('ENS_us', 'movie2u', 0x80a01, 0x7647000),
  @('hm_wlaa_175_us', 'movie3u', 0x0, 0x916000), @('hm_edaa_191_us', 'movie3u', 0x122c, 0x1040800),
  @('hm_edaa_190_us', 'movie3u', 0x32ad, 0x199f000), @('ENA_us', 'movie3u', 0x65eb, 0x5e18e000),
  @('hm_ptaa_200_us', 'movie3u', 0xc2907, 0xb1b800), @('hm_ptaa_005_us', 'movie3u', 0xc3f3e, 0x38a8000),
  @('hm_ptaa_006_us', 'movie3u', 0xcb08e, 0x38cf000), @('hm_ptaa_150_us', 'movie3u', 0xd222c, 0x2d23000),
  @('hm_ptaa_020_us', 'movie3u', 0xd7c72, 0x3d33000), @('hm_ptaa_130_us', 'movie3u', 0xdf6d8, 0x683c800),
  @('hm_ptaa_120_us', 'movie3u', 0xec751, 0x56f4000), @('hm_edaa_020_us', 'movie3u', 0xf7539, 0x65d800)
)
$script:Ps3MoviePacks = @{ movieu = 1937020928; movie2u = 1203009536; movie3u = 2081398784 }

# The PS3 movies, read from the disc image directly; if this reader gets a pack's size wrong,
# through Windows' own reading of the image (mounted for this step, unmounted afterwards).
function Step-Movies($state, $p) {
  $script:Ps3Mount = $null
  try { Step-MoviesRead $state $p }
  finally {
    if ($script:Ps3Mount) {
      Dismount-DiskImage -ImagePath $script:Ps3Mount -ErrorAction SilentlyContinue | Out-Null
      $script:Ps3Mount = $null
    }
  }
}

function Get-MountedPs3([string]$iso) {
  if (-not (Get-Command Mount-DiskImage -ErrorAction SilentlyContinue)) { return $null }
  try {
    $img = Get-DiskImage -ImagePath $iso -ErrorAction Stop
    if (-not $img.Attached) {
      Say '  opening the PS3 disc image through Windows...'
      $img = Mount-DiskImage -ImagePath $iso -PassThru -ErrorAction Stop
      $script:Ps3Mount = $iso
    }
    $letter = ($img | Get-Volume -ErrorAction Stop).DriveLetter
    if ($letter) { return "${letter}:\" }
  } catch { Say "  Windows could not open the disc image either ($($_.Exception.Message))" 'Yellow' }
  return $null
}

function Step-MoviesRead($state, $p) {
  $ps3 = $state.inputs.ps3
  $ffmpeg = Get-Ffmpeg $p.tools
  $outDir = Join-Path $p.install 'movies_ps3'
  New-Item -ItemType Directory -Force -Path $outDir, $p.work | Out-Null
  # Where each pack is: inside the disc image (read in place) or a file of the disc folder.
  $packs = @{}
  $script:Ps3Drive = $null
  foreach ($k in $script:Ps3MoviePacks.Keys) {
    $rel = "PS3_GAME/USRDIR/weiss_data/movie/$k.ps3.bin"
    if ($ps3.kind -eq 'iso') {
      $at = [LrIso]::Locate($ps3.path, $rel)
      $packs[$k] = @{ file = $ps3.path; offset = $at[0]; size = $at[1] }
      if ($at[0] -eq -2 -and $at[1] -eq $script:Ps3MoviePacks[$k]) {
        # Stored in pieces that are not one after the other: copied out once (about 2 GB).
        $copy = Join-Path $p.work "$k.ps3.bin"
        if (-not ((Test-Path -LiteralPath $copy) -and (Get-Item -LiteralPath $copy).Length -eq $at[1])) {
          Say "  copying $k.ps3.bin out of the PS3 disc image (it is stored in pieces)..."
          $job = Start-Job -ScriptBlock { param($iso, $r, $d, $isoPs1) . $isoPs1; [LrIso]::Extract($iso, $r, $d) } -ArgumentList $ps3.path, $rel, $copy, (Join-Path $PSScriptRoot 'iso.ps1')
          try {
            Wait-WithProgress { [bool](Wait-Job $job -Timeout 15) } { '{0:N1} of {1:N1} GB' -f ((Get-Item -LiteralPath $copy -ErrorAction SilentlyContinue).Length / 1GB), ($at[1] / 1GB) }
            Receive-Job $job -ErrorAction Stop | Out-Null
          } finally { Remove-Job $job -Force }
        }
        $packs[$k] = @{ file = $copy; offset = 0; size = (Get-Item -LiteralPath $copy).Length }
      }
    } else {
      $f = Join-Path $ps3.path ($rel -replace '/', '\')
      $packs[$k] = @{ file = $f; offset = 0; size = $(if (Test-Path -LiteralPath $f) { (Get-Item -LiteralPath $f).Length } else { -1 }) }
    }
    if ($packs[$k].size -ne $script:Ps3MoviePacks[$k] -and $ps3.kind -eq 'iso') {
      if (-not $script:Ps3Drive) { $script:Ps3Drive = Get-MountedPs3 $ps3.path }
      $f = $(if ($script:Ps3Drive) { Join-Path $script:Ps3Drive ($rel -replace '/', '\') } else { '' })
      if ($f -and (Test-Path -LiteralPath $f)) {
        Say ("  {0}.ps3.bin: {1:N0} bytes read from the image directly, {2:N0} through Windows" -f $k, $packs[$k].size, (Get-Item -LiteralPath $f).Length)
        $packs[$k] = @{ file = $f; offset = 0; size = (Get-Item -LiteralPath $f).Length }
      }
    }
    if ($packs[$k].size -ne $script:Ps3MoviePacks[$k]) {
      throw ("$k.ps3.bin on this PS3 disc is {0:N0} bytes, not the {1:N0} of the USA disc (BLUS31311), the only one supported for the PS3 movies so far." -f $packs[$k].size, $script:Ps3MoviePacks[$k])
    }
  }
  $tmp = Join-Path $p.work 'movie.pam'
  $buf = New-Object byte[] (4MB)
  $i = 0; $failed = @()
  foreach ($m in $script:Ps3Movies) {
    $i++
    $name, $pack, $sector, $size = $m
    $out = Join-Path $outDir "$name.mp4"
    if ((Test-Path -LiteralPath $out) -and (Get-Item -LiteralPath $out).Length -gt 0) { continue }
    $pk = $packs[$pack]
    $src = [IO.File]::Open($pk.file, 'Open', 'Read', 'Read')
    $dst = [IO.File]::Create($tmp)
    try {
      $src.Position = $pk.offset + [int64]$sector * 2048
      $left = [int64]$size
      $first = $true
      while ($left -gt 0) {
        $n = $src.Read($buf, 0, [int][math]::Min($buf.Length, $left))
        if ($n -le 0) { throw "Short read in $pack.ps3.bin at $name" }
        if ($first -and [Text.Encoding]::ASCII.GetString($buf, 0, 4) -ne 'PAMF') { throw "$name does not start where expected; this PS3 disc is not the supported one" }
        # The PAMF header's length (sectors, at byte 8): ENA_us has a stray pack inside its
        # header that ffmpeg 7.1 cannot get past (no frame size); its data starts after it.
        if ($first) { $header = (([int64]$buf[8] -shl 24) -bor ([int64]$buf[9] -shl 16) -bor ([int64]$buf[10] -shl 8) -bor [int64]$buf[11]) * 2048 }
        $first = $false
        $dst.Write($buf, 0, $n)
        $left -= $n
      }
    } finally { $src.Close(); $dst.Close() }
    $ok = $false
    foreach ($a in @(@('-f', 'mpeg', '-i', $tmp, '-map', '0:v:0'),
                     @('-probesize', '1G', '-analyzeduration', '600M', '-f', 'mpeg', '-i', $tmp, '-map', '0:v:0'),
                     @('-probesize', '1G', '-analyzeduration', '600M', '-f', 'mpeg', '-i', $tmp, '-map', '0:v:1'),
                     @('-probesize', '1G', '-analyzeduration', '600M', '-f', 'mpeg', '-i', $tmp, '-map', '0:v:2'),
                     @('-skip_initial_bytes', "$header", '-f', 'mpeg', '-i', $tmp, '-map', '0:v:0'),
                     @('-skip_initial_bytes', "$header", '-probesize', '1G', '-analyzeduration', '600M', '-f', 'mpeg', '-i', $tmp, '-map', '0:v:0'))) {
      if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }
      $rc = Invoke-Logged $ffmpeg (@('-hide_banner', '-loglevel', 'error', '-nostdin', '-y') + $a + @('-c', 'copy', '-an', '-sn', '-movflags', '+faststart', $out)) (Join-Path $p.logs "movie_$name.log") -Quiet
      if ($rc -eq 0 -and (Test-Path -LiteralPath $out) -and (Get-Item -LiteralPath $out).Length -gt 64KB) { $ok = $true; break }
    }
    Remove-Item -LiteralPath $tmp -Force
    if (-not $ok) {
      Say "  (last lines of $(Join-Path $p.logs "movie_$name.log"))" 'Yellow'
      Get-Content -LiteralPath (Join-Path $p.logs "movie_$name.log") -Tail 6 | ForEach-Object { Say "    $_" 'Yellow' }
    }
    if ($ok) { Say ("  [{0,2}/21] {1}" -f $i, $name) } else { $failed += $name; Say ("  [{0,2}/21] {1}: could not convert (stays the 360 version)" -f $i, $name) 'Yellow' }
  }
  Copy-Item -LiteralPath $ffmpeg -Destination (Join-Path $p.install 'ffmpeg.exe') -Force  # the game plays them through ffmpeg
  if ($failed.Count -eq $script:Ps3Movies.Count) { throw "None of the PS3 movies converted; see $($p.logs)\movie_*.log" }
}

function Step-JpVoice($state, $p) {
  $steam = $state.inputs.steam.path
  $ffmpeg = Get-Ffmpeg $p.tools
  $exe = Join-Path $p.work 'lr_jpvoice.exe'
  Build-Converter $script:T (Join-Path $script:Repo 'converters\jpvoice\lr_jpvoice.cpp') $exe
  $out = Join-Path $p.install 'jpvoice'
  $tmp = "$out.new"
  if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
  Say '  converting the Japanese voices, lip-sync, and text (10-30 minutes)...'
  $rc = Invoke-Logged $exe @('--ffmpeg', $ffmpeg, $p.game, $steam, $tmp, (Join-Path $p.work 'jpvoice_samples')) (Join-Path $p.logs 'jpvoice.log')
  if ($rc -ne 0 -and $rc -ne 3) { throw "Converting the Japanese voices failed; see $($p.logs)\jpvoice.log" }
  if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
  Move-Item -LiteralPath $tmp -Destination $out
  Set-Content -LiteralPath (Join-Path $out 'version.txt') -Value '6'
  if ($rc -eq 3) { Say '  fewer than half of the voice files converted; the others stay English' 'Yellow' }
  # Japanese voices to start with when they are installed (Settings > Voice Language changes it).
  $launch = Join-Path $p.install 'lightningreturns_launch.txt'
  if ((Test-Path -LiteralPath $launch) -and -not (Select-String -LiteralPath $launch -Pattern '^LR_VOICE_DEFAULT' -Quiet)) {
    Add-Content -LiteralPath $launch -Value 'LR_VOICE_DEFAULT = jp'
  }
}

function Step-Dlc($state, $p) {
  $script = Join-Path $script:Repo 'scripts\LR_Import_360_Content.ps1'
  foreach ($pkg in @($state.inputs.dlc360.packages)) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $script -Source $pkg -UserData $p.userData | ForEach-Object { Say "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "Installing the DLC package $pkg failed (see above)." }
  }
}

# Japanese voice lines of the installed DLC packs (Steam weiss_data\dlc\<pack>\..img_v), added to
# the Japanese patch; runs after the DLC so every pack installed by then is covered.
function Step-DlcVoice($state, $p) {
  $out = Join-Path $p.install 'jpvoice'
  if (-not (Test-Path -LiteralPath (Join-Path $out 'filelist_patch_u.x360.bin'))) { Say '  no Japanese voices installed; skipped'; return }
  $ffmpeg = Get-Ffmpeg $p.tools
  $exe = Join-Path $p.work 'lr_jpvoice.exe'
  if (-not (Test-Path -LiteralPath $exe)) { Build-Converter $script:T (Join-Path $script:Repo 'converters\jpvoice\lr_jpvoice.cpp') $exe }
  $rc = Invoke-Logged $exe @('--ffmpeg', $ffmpeg, '--dlc', $out, $state.inputs.steam.path, $p.userData) (Join-Path $p.logs 'jpvoice_dlc.log')
  if ($rc -eq 2) { Say '  no DLC voice lines to convert (no DLC installed, or none with a Steam Japanese version)'; return }
  if ($rc -ne 0) { throw "Adding the Japanese DLC voices failed; see $($p.logs)\jpvoice_dlc.log" }
}

# ---------------------------------------------------------------------------------------------
function Get-Steps($state) {
  $windows = $state.target -in 'windows', 'both'
  $android = $state.target -in 'android', 'both'
  $s = [ordered]@{}
  $s['tools'] = @{ title = 'Build tools'; run = ${function:Step-Tools}; always = $true }
  $s['game'] = @{ title = 'Xbox 360 game files'; run = ${function:Step-Game} }
  $s['icon'] = @{ title = 'Icon (Lightning from the title screen)'; run = ${function:Step-Icon} }
  $s['project'] = @{ title = 'Project'; run = ${function:Step-Project} }
  $s['codegen'] = @{ title = 'Recompiling the game code'; run = ${function:Step-Codegen} }
  if ($windows) {
    $s['exe'] = @{ title = 'Building the game (Windows)'; run = ${function:Step-Exe} }
    $s['plugin'] = @{ title = 'Building the renderer (Windows)'; run = ${function:Step-Plugin} }
    $s['install'] = @{ title = 'Game folder'; run = ${function:Step-Install} }
    $s['settings'] = @{ title = 'Settings'; run = ${function:Step-Settings} }
    $s['fonts'] = @{ title = 'Sharp fonts'; run = ${function:Step-Fonts} }
  }
  if ($state.inputs.ps3) {
    $s['ps3'] = @{ title = 'PS3 buttons / UI'; run = ${function:Step-Ps3Ui} }
    if ($windows) { $s['movies'] = @{ title = 'PS3 cinematics'; run = ${function:Step-Movies} } }
  }
  if ($state.inputs.steam -and $windows) { $s['steam'] = @{ title = 'Japanese voices (Steam)'; run = ${function:Step-JpVoice} } }
  if ($state.inputs.dlc360) { $s['dlc'] = @{ title = '360 DLC'; run = ${function:Step-Dlc} } }
  if ($state.inputs.steam -and $windows) { $s['dlcvoice'] = @{ title = 'Japanese DLC voices (Steam)'; run = ${function:Step-DlcVoice} } }
  if ($android) {
    $s['android'] = @{ title = 'Building the game (Android)'; run = ${function:Step-Android} }
    $s['phone'] = @{ title = 'Installing on the phone'; run = ${function:Step-Phone} }
  }
  return $s
}

# Files dated in the future (zip times have no time zone: a zip made where it is already
# tomorrow unpacks with tomorrow's dates) make CMake regenerate its build forever ("build.ninja
# still dirty"). Such dates become now: the builder's own files, and the sources copied from them.
function Reset-FutureTimes([string[]]$dirs) {
  $now = Get-Date
  $n = 0
  foreach ($d in $dirs) {
    if (-not (Test-Path -LiteralPath $d)) { continue }
    foreach ($f in Get-ChildItem -LiteralPath $d -Recurse -File -Force -ErrorAction SilentlyContinue) {
      if ($f.LastWriteTime -gt $now) { $f.LastWriteTime = $now; $n++ }
    }
  }
  if ($n) { Say "  $n file(s) were dated in the future (zip time zones); dated now" }
}

function Invoke-Pipeline($state, [string[]]$Only = @()) {
  $p = Get-Paths $state
  New-Item -ItemType Directory -Force -Path $p.logs, $p.work, $p.tools | Out-Null
  $sources = @($script:Repo, $p.plugin) + @('src', 'analysis', 'generated' | ForEach-Object { Join-Path $p.project $_ })
  Reset-FutureTimes $sources
  foreach ($f in Get-ChildItem -LiteralPath $p.project -File -ErrorAction SilentlyContinue) { if ($f.LastWriteTime -gt (Get-Date)) { $f.LastWriteTime = Get-Date } }
  Save-BuildState $state
  $steps = Get-Steps $state
  # Only some parts again (adding things later): those steps, after the tools.
  if ($Only) { $state.done = @($state.done | Where-Object { $Only -notcontains $_ }) }
  $n = 0
  foreach ($key in $steps.Keys) {
    $n++
    $step = $steps[$key]
    if ($Only -and $Only -notcontains $key -and -not $step.always) { continue }
    if (-not $step.always -and $state.done -contains $key) { continue }
    Say ''
    Say ("[{0}/{1}] {2}" -f $n, $steps.Count, $step.title) 'Cyan'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
      & $step.run $state $p
    } catch {
      Say ''
      Say "Stopped at: $($step.title)" 'Red'
      Say "  $($_.Exception.Message)" 'Red'
      Say 'Fix that and run the builder again: it carries on from this step.' 'Red'
      Save-BuildState $state
      exit 1
    }
    if (-not $step.always) { $state.done = @($state.done) + $key; Save-BuildState $state }
    Say ("  done ({0:mm\:ss})" -f $sw.Elapsed) 'Green'
  }
  $state.finished = $true
  Save-BuildState $state
  Say ''
  Say 'All done.' 'Green'
  if ($state.target -in 'windows', 'both') { Say "  Play: $($p.install)\lightningreturns.exe (double-click)" 'Green' }
  Say "  Logs: $($p.logs)"
}
