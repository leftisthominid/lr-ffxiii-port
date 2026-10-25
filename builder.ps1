<#
  Lightning Returns builder. Start it with "Build Lightning Returns.bat" (double-click).

  It takes your own copies of the game and builds the port from them: it checks them, unpacks
  the Xbox 360 game, recompiles it, builds the game for Windows and/or Android, converts what
  the PS3 and Steam versions add, and installs it. Your game files are only ever read.

  Running it again after a stop carries on from the last finished step; with a finished build
  in the output folder it offers to add things (360 DLC, PS3 disc, Steam version) or to install
  on a phone.
#>
param([string]$Output = '')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Here = $PSScriptRoot
. (Join-Path $Here 'builder\iso.ps1')
. (Join-Path $Here 'builder\inputs.ps1')
. (Join-Path $Here 'builder\pipeline.ps1')

function Say([string]$text, [string]$color = '') {
  if ($color) { Write-Host $text -ForegroundColor $color } else { Write-Host $text }
}

# y / yes / n / no, any case. Anything else asks again (not counted as an answer).
function Ask-YesNo([string]$question) {
  while ($true) {
    $a = (Read-Host "$question (y/n)").Trim().ToLowerInvariant()
    if ($a -eq 'y' -or $a -eq 'yes') { return $true }
    if ($a -eq 'n' -or $a -eq 'no') { return $false }
    Say 'Please answer y or n.' 'Yellow'
  }
}

function Ask-Choice([string]$question, [string[]]$options) {
  Say $question
  for ($i = 0; $i -lt $options.Count; ++$i) { Say ("  [{0}] {1}" -f ($i + 1), $options[$i]) }
  while ($true) {
    $a = (Read-Host ("Choose {0}" -f (((1..$options.Count) -join ', ') -replace ', (\d+)$', ' or $1'))).Trim()
    $n = 0
    if ([int]::TryParse($a, [ref]$n) -and $n -ge 1 -and $n -le $options.Count) { return $n }
    Say ("Please type a number from 1 to {0}." -f $options.Count) 'Yellow'
  }
}

function Show-Opening {
  Clear-Host
  Say 'Lightning Returns: Final Fantasy XIII - Triumfulgurate Edition - builder' 'Cyan'
  Say ''
  if (Ask-YesNo 'Lightning Farron is bae.') {
    Say 'Correct.' 'Green'
  } elseif (Ask-YesNo 'That looks like a typo. Lightning Farron is bae.') {
    Say 'Correct now.' 'Green'
  } else {
    Say 'You are in the wrong place. Hopefully playing this game teaches you that your answer was wrong.' 'Magenta'
  }
  Say ''
  Say 'Before you go on, know how this was made.'
  Say ''
  Say 'This decompilation / recompilation project was principally coded by an LLM (Claude, by'
  Say 'Anthropic). Another LLM (ChatGPT, by OpenAI) did the initial setup. The person who steered'
  Say 'it has nowhere near the coding experience it would have taken to do this without generative'
  Say 'AI.'
  Say ''
  if (-not (Ask-YesNo 'Are you OK with this?')) {
    Say 'OK. Nothing was changed or written. Goodbye.'
    Start-Sleep -Seconds 2
    exit 10
  }
}

# ---------------------------------------------------------------------------------------------
Show-Opening

if (-not $Output) { $Output = Find-ExistingOutput }
$state = $null
if ($Output) { $state = Read-BuildState $Output }

if ($state -and $state.finished) {
  # Steps a newer builder added since this build finished: done first.
  $steps = Get-Steps $state
  $new = @($steps.Keys | Where-Object { -not $steps[$_].always -and $_ -ne 'phone' -and $state.done -notcontains $_ })
  if ($new.Count) {
    Say ''
    Say ("This builder adds to your build: {0}" -f (($new | ForEach-Object { $steps[$_].title }) -join ', ')) 'Cyan'
    Invoke-Pipeline $state -Only $new
  }
  # Game code newer than this build's (a newer builder): the game and the renderer are built
  # again from it; the recompiled code, game files, movies, voices, and DLC stay.
  if ($state.codeHash -ne (Get-CodeHash $state)) {
    $code = @('project')
    if ($state.target -in 'windows', 'both') { $code += @('exe', 'plugin', 'install') }
    if ($state.done -contains 'android') { $code += 'android' }
    Say ''
    Say 'This builder has newer game code than your build: building the game and the renderer again' 'Cyan'
    Say '(the recompiled code, the game files, movies, voices, and DLC are kept).' 'Cyan'
    Invoke-Pipeline $state -Only $code
    if ($state.done -contains 'phone') { Say 'On a phone: choose "Install on a phone" below to put the new app on it.' 'Cyan' }
  }
  Say ''
  $c = Ask-Choice 'Lightning Returns is already built here. What do you want to do?' @(
    'Add or update the 360 DLC', 'Add the PS3 disc (cinematics, PS3 buttons)', 'Add the Steam version (Japanese voices)',
    'Install on a phone', 'Rebuild everything', 'Nothing - close')
  switch ($c) {
    1 { $state.inputs.dlc360 = Ask-Dlc360 -Required; Save-BuildState $state; Invoke-Pipeline $state -Only @('dlc', 'phone') }
    2 { $state.inputs.ps3 = Ask-Ps3 -Required; Save-BuildState $state; Invoke-Pipeline $state -Only @('ps3', 'movies', 'android', 'phone') }
    3 { $state.inputs.steam = Ask-Steam -Required; Save-BuildState $state; Invoke-Pipeline $state -Only @('steam') }
    4 {
      # A Windows-only build has no app yet: build it first.
      if ($state.done -notcontains 'android') { $state.target = 'both'; Save-BuildState $state; Invoke-Pipeline $state -Only @('android', 'phone') }
      else { Invoke-Pipeline $state -Only @('phone') }
    }
    5 { $state.done = @(); $state.finished = $false; Invoke-Pipeline $state }
    6 { }
  }
  exit 0
}

if (-not $state) {
  Say ''
  $t = Ask-Choice 'What do you want to build?' @('Windows (PC)', 'Android (phone / handheld with a Snapdragon / Adreno GPU)', 'Both')
  $state = New-BuildState @('windows', 'android', 'both')[$t - 1]
  Say ''
  $state.inputs.game360 = Ask-Game360
  $state.inputs.ps3 = Ask-Ps3
  $state.inputs.steam = Ask-Steam
  $state.inputs.dlc360 = Ask-Dlc360
  $state.output = Ask-Output $state
  if ($state.target -in 'windows', 'both') {
    $state.shortcut = Ask-YesNo 'Put a "Lightning Returns" shortcut on the desktop when it is built?'
  }
} else {
  Say ''
  Say "Carrying on with the build in $($state.output) (finished so far: $(@($state.done).Count) step(s))." 'Cyan'
}
Invoke-Pipeline $state
