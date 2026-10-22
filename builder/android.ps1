# Lightning Returns builder - the Android build (arm64, Vulkan): the toolchain, the ReXGlue SDK
# for Android, the renderer, the game (libmain.so), the APK, and installing it on the phone.
# Runs on Windows; also on Linux (the same steps, used to test the builder). Dot-sourced by
# pipeline.ps1.

$script:OnWindows = ($PSVersionTable.PSEdition -eq 'Desktop') -or $IsWindows
$script:Exe = $(if ($script:OnWindows) { '.exe' } else { '' })
$script:Bat = $(if ($script:OnWindows) { '.bat' } else { '' })

$script:AndroidDownloads = [ordered]@{
  jdk = @{ name = 'Java 17 (Eclipse Temurin)'; size = 190520081
    url = 'https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.16%2B8/OpenJDK17U-jdk_x64_windows_hotspot_17.0.16_8.zip'
    sha256 = '8C7CFFF78A55C56EBAF470ED6A89C6466B47D8274BDABDDA997D7507C20325C5' }
  git = @{ name = 'Git (MinGit 2.51.0)'; size = 41268410
    url = 'https://github.com/git-for-windows/git/releases/download/v2.51.0.windows.1/MinGit-2.51.0-64-bit.zip'
    sha256 = 'C2C955A21FA99889D83F485F24FA5D9A38FFFC2D509D4022385510E11C26B250' }
}
foreach ($k in $script:AndroidDownloads.Keys) { $script:Downloads[$k] = $script:AndroidDownloads[$k] }

# The ReXGlue SDK release the port is built on, and the third-party code its Android build needs,
# each at the exact commit the SDK pins.
$script:SdkRepo = 'https://github.com/rexglue/rexglue-sdk'
$script:SdkCommit = 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518'  # v0.10.0
$script:SdkSubmodules = [ordered]@{
  'cli11' = @('https://github.com/CLIUtils/CLI11', 'bfffd37e1f804ca4fae1caae106935791696b6a9')
  'libmspack' = @('https://github.com/kyz/libmspack.git', '305907723a4e7ab2018e58040059ffb5e77db837')
  'FFmpeg' = @('https://github.com/wmarti/FFmpeg.git', '0604b464c7cb4ebc94940cf1f324a3b26b87717c')
  'tomlplusplus' = @('https://github.com/marzer/tomlplusplus.git', '30172438cee64926dc41fdd9c11fb3ba5b2ba9de')
  'simde' = @('https://github.com/simd-everywhere/simde.git', '71fd833d9666141edcd1d3c109a80e228303d8d7')
  'xxHash' = @('https://github.com/Cyan4973/xxHash.git', 'e626a72bc2321cd320e953a0ccf1584cad60f363')
  'spdlog' = @('https://github.com/gabime/spdlog.git', '79524ddd08a4ec981b7fea76afd08ee05f83755d')
  'fmt' = @('https://github.com/fmtlib/fmt.git', '407c905e45ad75fc29bf0f9bb7c5c2fd3475976f')
  'utfcpp' = @('https://github.com/nemtrif/utfcpp.git', '63d64de49fd6b829f7c8694df5ab2ee625cb7134')
  'imgui' = @('https://github.com/ocornut/imgui.git', '6d910d5487d11ca567b61c7824b0c78c569d62f0')
  'sdl3' = @('https://github.com/libsdl-org/SDL', '8bf3b7215ad9fc3deb583c6a3a37c6c67f2e24e4')
  'vulkan-headers' = @('https://github.com/KhronosGroup/Vulkan-Headers.git', 'e3b1eec08173d6b825cd3ac88c885a63b621504a')
  'vulkan-memory-allocator' = @('https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git', '1d8f600fd424278486eade7ed3e877c99f0846b1')
  'spirv-headers' = @('https://github.com/KhronosGroup/SPIRV-Headers.git', '29981f65241605e08b0ede4cfeb999fe3b723c6a')
  'spirv-tools' = @('https://github.com/KhronosGroup/SPIRV-Tools.git', '9a49b0883b9b635689a85b5647dbfcb223268151')
  'glslang' = @('https://github.com/KhronosGroup/glslang.git', 'f4f1d8a352ca1908943aea2ad8c54b39b4879080')
  'o1heap' = @('https://github.com/pavel-kirienko/o1heap.git', '388a73fd9007300e5130c5fe352d9ce3288b6dde')
  'inja' = @('https://github.com/pantor/inja.git', '7d1b4600b68595085a949743331c2e5673f511ea')
}
# Google's Android packages (checked against the SHA-1 Google lists for each in its repository).
$script:GooglePackages = [ordered]@{
  'ndk;28.2.13676358' = 'ndk\28.2.13676358'
  'build-tools;35.0.0' = 'build-tools\35.0.0'
  'platforms;android-35' = 'platforms\android-35'
  'platform-tools' = 'platform-tools'
}
$script:NdkVersion = '28.2.13676358'

function Get-AndroidPaths($p) {
  $a = Join-Path $p.out 'android'
  return @{ root = $a; sdk = Join-Path $a 'sdk'; src = Join-Path $a 'src\rexglue-sdk'; rexsdk = Join-Path $a 'rexsdk'
            build = Join-Path $a 'build'; apk = Join-Path $a 'LightningReturns.apk'; keystore = Join-Path $a 'debug.keystore' }
}

function Expand-ZipTo([string]$zip, [string]$dest) {
  $tmp = "$dest.unpack"
  if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $tmp | Out-Null
  Expand-ZipWithProgress $zip $tmp
  $items = @(Get-ChildItem -LiteralPath $tmp)
  $from = $tmp
  if ($items.Count -eq 1 -and $items[0].PSIsContainer) { $from = $items[0].FullName }
  if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
  New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
  Move-Item -LiteralPath $from -Destination $dest
  if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
}

# Google's packages from dl.google.com, each checked against Google's repository listing.
# The Android SDK license is shown once and has to be accepted, as sdkmanager asks.
function Install-GooglePackages([string]$sdk, [string]$downloads) {
  $missing = @($script:GooglePackages.Keys | Where-Object { -not (Test-Path -LiteralPath (Join-Path $sdk $script:GooglePackages[$_])) })
  if (-not $missing.Count) { return }
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  $base = 'https://dl.google.com/android/repository/'
  $xml = [xml](New-Object Net.WebClient).DownloadString($base + 'repository2-3.xml')
  $accepted = Join-Path $sdk 'licenses\android-sdk-license.accepted'
  if (-not (Test-Path -LiteralPath $accepted)) {
    $license = ($xml.SelectNodes('//license') | Where-Object { $_.id -eq 'android-sdk-license' } | Select-Object -First 1).'#text'
    $file = Join-Path $downloads 'android-sdk-license.txt'
    New-Item -ItemType Directory -Force -Path $downloads | Out-Null
    Set-Content -LiteralPath $file -Value $license -Encoding UTF8
    Say ''
    Say "The Android build needs Google's Android SDK packages (NDK, build tools, platform). Their license"
    Say "is Google's Android Software Development Kit License Agreement (full text: $file)."
    if (-not (Ask-YesNo 'Do you accept it?')) { throw 'The Android SDK license was not accepted; the Android build needs it.' }
    New-Item -ItemType Directory -Force -Path (Split-Path $accepted) | Out-Null
    Set-Content -LiteralPath $accepted -Value (Get-Date -Format s)
  }
  $hostOs = $(if ($script:OnWindows) { 'windows' } else { 'linux' })
  foreach ($pkg in $missing) {
    $node = $xml.SelectNodes('//remotePackage') | Where-Object { $_.path -eq $pkg } | Select-Object -First 1
    if (-not $node) { throw "Google's repository does not list $pkg any more." }
    $archive = $node.archives.archive | Where-Object { -not $_.'host-os' -or $_.'host-os' -eq $hostOs } | Select-Object -First 1
    $url = $base + $archive.complete.url
    $zip = Join-Path $downloads $archive.complete.url
    $sha1 = $archive.complete.checksum.'#text'
    if (-not $sha1) { $sha1 = $archive.complete.checksum }
    if (-not ((Test-Path -LiteralPath $zip) -and (Get-FileHash -LiteralPath $zip -Algorithm SHA1).Hash -eq $sha1.ToUpperInvariant())) {
      Say ("  downloading {0} ({1:N0} MB)..." -f $pkg, ([double]$archive.complete.size / 1MB))
      Invoke-Download $url "$zip.part" ([long]$archive.complete.size)
      if ((Get-FileHash -LiteralPath "$zip.part" -Algorithm SHA1).Hash -ne $sha1.ToUpperInvariant()) {
        Remove-Item -LiteralPath "$zip.part" -Force
        throw "The download of $pkg does not match Google's checksum; run the builder again."
      }
      Move-Item -LiteralPath "$zip.part" -Destination $zip -Force
    }
    Say "  unpacking $pkg..."
    Expand-ZipTo $zip (Join-Path $sdk $script:GooglePackages[$pkg])
  }
}

# -> @{ java; javac; keytool; git; sdk; ndk; bt; jar; adb; cmake; ninja }
function Get-AndroidTools($p) {
  $a = Get-AndroidPaths $p
  $dl = Join-Path $p.tools 'downloads'
  if ($script:OnWindows) {
    Expand-Pinned 'cmake' $dl (Join-Path $p.tools 'cmake') 'bin\cmake.exe'
    Expand-Pinned 'ninja' $dl (Join-Path $p.tools 'ninja') 'ninja.exe'
    Expand-Pinned 'jdk' $dl (Join-Path $p.tools 'jdk') 'bin\javac.exe'
    Expand-Pinned 'git' $dl (Join-Path $p.tools 'git') 'cmd\git.exe'
    Install-GooglePackages $a.sdk $dl
    $jdk = Join-Path $p.tools 'jdk\bin'
    $t = @{ cmake = Join-Path $p.tools 'cmake\bin\cmake.exe'; ninja = Join-Path $p.tools 'ninja\ninja.exe'
            java = Join-Path $jdk 'java.exe'; javac = Join-Path $jdk 'javac.exe'; keytool = Join-Path $jdk 'keytool.exe'
            git = Join-Path $p.tools 'git\cmd\git.exe'; sdk = $a.sdk }
  } else {
    # Linux (testing the builder): the tools on PATH and an existing SDK ($env:LR_ANDROID_SDK).
    $sdk = $(if ($env:LR_ANDROID_SDK) { $env:LR_ANDROID_SDK } else { $a.sdk })
    $t = @{ cmake = (Get-Command cmake).Source; ninja = (Get-Command ninja).Source; java = (Get-Command java).Source
            javac = (Get-Command javac).Source; keytool = (Get-Command keytool).Source; git = (Get-Command git).Source; sdk = $sdk }
  }
  $t.ndk = Join-Path $t.sdk "ndk\$script:NdkVersion"
  $t.bt = Join-Path $t.sdk 'build-tools\35.0.0'
  $t.jar = Join-Path $t.sdk 'platforms\android-35\android.jar'
  $t.adb = Join-Path $t.sdk "platform-tools\adb$script:Exe"
  $hostTag = $(if ($script:OnWindows) { 'windows-x86_64' } else { 'linux-x86_64' })
  $t.strip = Join-Path $t.ndk "toolchains\llvm\prebuilt\$hostTag\bin\llvm-strip$script:Exe"
  $t.libcxx = Join-Path $t.ndk "toolchains\llvm\prebuilt\$hostTag\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so"
  foreach ($k in 'cmake', 'ninja', 'java', 'javac', 'keytool', 'git', 'jar', 'strip', 'libcxx') {
    if (-not (Test-Path -LiteralPath $t[$k])) { throw "Android build tool missing: $($t[$k])" }
  }
  return $t
}

function Invoke-Git($t, [string[]]$gitArgs, [string]$cwd) {
  $rc = Invoke-Logged $t.git $gitArgs (Join-Path $script:AndroidLogDir 'git.log') $cwd
  if ($rc -ne 0) { throw "git $($gitArgs -join ' ') failed (no internet connection, or GitHub unreachable)." }
}

# Symbolic links in a git checkout: git on Windows (without Developer Mode) writes each one as a
# small text file holding the link's target path, which the compiler then reads as source code
# (libmspack's cabextract\mspack\*.c are links). Each such file becomes a copy of its target.
function Repair-GitLinks($t, [string]$repo) {
  $log = Join-Path $script:AndroidLogDir 'git_links.log'
  $rc = Invoke-Logged $t.git @('-c', 'core.quotepath=off', 'ls-files', '-s') $log $repo
  if ($rc -ne 0) { throw "git could not list the files of $repo; see $log" }
  $links = @(Get-Content -LiteralPath $log | Where-Object { $_ -match '^120000 [0-9a-f]+ \d+\t' } | ForEach-Object { ($_ -split "`t", 2)[1] })
  $fixed = 0
  $done = @{}
  $bad = [IO.Path]::GetInvalidPathChars()
  for ($pass = 0; $pass -lt 3; ++$pass) {  # a link to a link resolves on a later pass
    foreach ($rel in $links) {
      if ($done[$rel]) { continue }
      $file = [IO.Path]::Combine($repo, $rel)
      $item = Get-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
      if (-not $item -or $item.LinkType -or $item.Length -gt 1024) { $done[$rel] = $true; continue }  # a real link
      $target = [IO.File]::ReadAllText($file).Trim()
      if (-not $target -or $target.Contains("`n") -or $target.IndexOfAny($bad) -ge 0) { $done[$rel] = $true; continue }
      $src = [IO.Path]::GetFullPath([IO.Path]::Combine((Split-Path $file), $target))
      $srcRel = $links | Where-Object { [IO.Path]::GetFullPath([IO.Path]::Combine($repo, $_)) -eq $src } | Select-Object -First 1
      if ($srcRel -and -not $done[$srcRel]) { continue }  # its target is a link not resolved yet
      if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { $done[$rel] = $true; continue }
      Copy-Item -LiteralPath $src -Destination $file -Force
      $done[$rel] = $true
      $fixed++
    }
  }
  return $fixed
}

function Repair-SdkLinks($t, $a) {
  $n = 0
  foreach ($repo in @($a.src) + @(Get-ChildItem -LiteralPath (Join-Path $a.src 'thirdparty') -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName '.git') } | ForEach-Object { $_.FullName })) {
    $n += Repair-GitLinks $t $repo
  }
  if ($n) { Say "  $n linked source file(s) made into copies (git on Windows writes links as text files)" }
}

# The ReXGlue SDK source at the release commit, its third-party code at the pinned commits, and
# the Android changes (builder\android\rexglue-sdk-android.patch).
function Get-SdkSource($t, $a) {
  if (Test-Path -LiteralPath (Join-Path $a.src '.lr-android-ready')) { Repair-SdkLinks $t $a; return }
  $tmp = "$($a.src).new"
  if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $tmp | Out-Null
  Say '  fetching the ReXGlue SDK source (v0.10.0)...'
  Invoke-Git $t @('init', '-q') $tmp
  Invoke-Git $t @('-c', 'core.longpaths=true', 'fetch', '-q', '--depth', '1', $script:SdkRepo, $script:SdkCommit) $tmp
  Invoke-Git $t @('-c', 'core.longpaths=true', 'checkout', '-q', 'FETCH_HEAD') $tmp
  foreach ($name in $script:SdkSubmodules.Keys) {
    $url, $sha = $script:SdkSubmodules[$name]
    $dir = Join-Path $tmp "thirdparty\$name"
    Say "  fetching $name..."
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Invoke-Git $t @('init', '-q') $dir
    Invoke-Git $t @('-c', 'core.longpaths=true', 'fetch', '-q', '--depth', '1', $url, $sha) $dir
    Invoke-Git $t @('-c', 'core.longpaths=true', 'checkout', '-q', 'FETCH_HEAD') $dir
  }
  Repair-SdkLinks $t @{ src = $tmp }
  Say '  applying the Android changes...'
  Invoke-Git $t @('apply', '--whitespace=nowarn', (Join-Path $script:Repo 'android\rexglue-sdk-android.patch')) $tmp
  if (Test-Path -LiteralPath $a.src) { Remove-Item -LiteralPath $a.src -Recurse -Force }
  Move-Item -LiteralPath $tmp -Destination $a.src
  Set-Content -LiteralPath (Join-Path $a.src '.lr-android-ready') -Value $script:SdkCommit
}

function Get-NdkCMakeArgs($t) {
  return @('-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$($t.ninja -replace '\\','/')",
    "-DCMAKE_TOOLCHAIN_FILE=$((Join-Path $t.ndk 'build\cmake\android.toolchain.cmake') -replace '\\','/')",
    '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29', '-DANDROID_STL=c++_shared', '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF', '-DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON')
}

function Invoke-CMakeStep($t, [string[]]$cmakeArgs, [string]$logName, [string]$what) {
  $rc = Invoke-Logged $t.cmake $cmakeArgs (Join-Path $script:AndroidLogDir $logName)
  if ($rc -ne 0) { throw "$what failed; see $(Join-Path $script:AndroidLogDir $logName)" }
}

function Step-Android($state, $p) {
  $script:AndroidLogDir = Join-Path $p.logs 'android'
  New-Item -ItemType Directory -Force -Path $script:AndroidLogDir | Out-Null
  $a = Get-AndroidPaths $p
  $t = Get-AndroidTools $p
  $script:AT = $t
  Get-SdkSource $t $a
  $jobs = "$([Environment]::ProcessorCount)"
  $ndk = Get-NdkCMakeArgs $t

  Say '  building the ReXGlue SDK for Android (10-30 minutes the first time)...'
  $b = Join-Path $a.build 'sdk'
  if (-not (Test-Path -LiteralPath (Join-Path $b 'build.ninja'))) {
    Invoke-CMakeStep $t (@('-S', $a.src, '-B', $b) + $ndk + @('-DREXGLUE_ENABLE_TRACY=OFF', '-DREXGLUE_USE_VULKAN=ON',
      "-DCMAKE_INSTALL_PREFIX=$($a.rexsdk -replace '\\','/')")) 'sdk_configure.log' 'Configuring the Android SDK build'
  }
  Invoke-CMakeStep $t @('--build', $b, '--parallel', $jobs) 'sdk_build.log' 'Building the Android SDK'
  Invoke-CMakeStep $t @('--install', $b) 'sdk_install.log' 'Installing the Android SDK build'

  Say '  building the renderer for Android...'
  # The SDK headers the Android changes touch (outside graphics), put in front of the renderer's
  # own copy of the SDK headers.
  $overlay = Join-Path $a.build 'plugin-overlay\include'
  if (Test-Path -LiteralPath $overlay) { Remove-Item -LiteralPath $overlay -Recurse -Force }
  $patch = Get-Content -LiteralPath (Join-Path $script:Repo 'android\rexglue-sdk-android.patch')
  foreach ($line in $patch) {
    if ($line -match '^\+\+\+ b/(include/.+)$' -and $Matches[1] -notmatch '^include/rex/graphics/') {
      $rel = $Matches[1] -replace '/', '\'
      $dest = Join-Path (Split-Path $overlay) $rel
      New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
      Copy-Item -LiteralPath (Join-Path $a.src $rel) -Destination $dest -Force
    }
  }
  $pb = Join-Path $a.build 'plugin'
  if (-not (Test-Path -LiteralPath (Join-Path $pb 'build.ninja'))) {
    Invoke-CMakeStep $t (@('-S', (Join-Path $script:Repo 'android\plugin'), '-B', $pb) + $ndk + @(
      "-DCMAKE_PREFIX_PATH=$($a.rexsdk -replace '\\','/')", "-DCMAKE_FIND_ROOT_PATH=$($a.rexsdk -replace '\\','/')",
      "-DLR_KIT_PLUGIN_DIR=$((Join-Path $script:Repo 'plugin') -replace '\\','/')",
      "-DLR_GLSLANG_SOURCE=$((Join-Path $a.src 'thirdparty\glslang') -replace '\\','/')",
      "-DLR_ANDROID_HEADER_OVERLAY=$($overlay -replace '\\','/')")) 'plugin_configure.log' 'Configuring the Android renderer build'
  }
  Invoke-CMakeStep $t @('--build', $pb, '--parallel', $jobs) 'plugin_build.log' 'Building the Android renderer'

  Say '  building the game for Android (all recompiled code: 20-60 minutes)...'
  $gb = Join-Path $a.build 'game'
  if (-not (Test-Path -LiteralPath (Join-Path $gb 'build.ninja'))) {
    Invoke-CMakeStep $t (@('-S', (Join-Path $script:Repo 'android\game'), '-B', $gb) + $ndk + @(
      "-DCMAKE_PREFIX_PATH=$($a.rexsdk -replace '\\','/')", "-DCMAKE_FIND_ROOT_PATH=$($a.rexsdk -replace '\\','/')",
      "-DLR_PROJECT_DIR=$($p.project -replace '\\','/')", '-DLR_RECOMP_LIMIT=0')) 'game_configure.log' 'Configuring the Android game build'
  }
  Invoke-CMakeStep $t @('--build', $gb, '--parallel', $jobs) 'game_build.log' 'Building the Android game'

  Say '  packaging the app...'
  $jni = Join-Path $a.build 'staged-jni\lib\arm64-v8a'
  if (Test-Path -LiteralPath (Split-Path (Split-Path $jni))) { Remove-Item -LiteralPath (Split-Path (Split-Path $jni)) -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $jni | Out-Null
  foreach ($lib in (Join-Path $gb 'libmain.so'), (Join-Path $a.rexsdk 'lib\librexruntime.so'),
                   (Join-Path $pb 'graphics\librexgpu-xenos.so'), $t.libcxx) {
    if (-not (Test-Path -LiteralPath $lib)) { throw "Missing after the build: $lib" }
    $rc = Invoke-Logged $t.strip @('--strip-unneeded', '-o', (Join-Path $jni (Split-Path $lib -Leaf)), $lib) (Join-Path $script:AndroidLogDir 'strip.log')
    if ($rc -ne 0) { throw "Stripping $lib failed" }
  }
  New-Apk $t $a (Split-Path $jni) $p.icon
  Say "  $($a.apk)"
}

# APK: resources (aapt2), Java (javac + d8: the app and SDL's Java glue), native libraries stored
# uncompressed, zipalign, signed with this PC's own debug key (made once).
function New-Apk($t, $a, [string]$libRoot, [string]$iconDir = '') {
  $w = Join-Path $a.build 'apk'
  if (Test-Path -LiteralPath $w) { Remove-Item -LiteralPath $w -Recurse -Force }
  foreach ($d in 'res', 'gen', 'classes', 'dex', 'java\org\libsdl\app', 'sdlres') { New-Item -ItemType Directory -Force -Path (Join-Path $w $d) | Out-Null }
  $proj = Join-Path $script:Repo 'android\project\main'
  $sdlApp = Join-Path $a.src 'thirdparty\sdl3\android-project\app\src\main'
  Copy-Item -Path (Join-Path $sdlApp 'java\org\libsdl\app\*.java') -Destination (Join-Path $w 'java\org\libsdl\app')
  Get-ChildItem -LiteralPath (Join-Path $sdlApp 'res') -Directory -Filter 'mipmap-*' | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $w 'sdlres') -Recurse }
  $manifest = [IO.File]::ReadAllText((Join-Path $proj 'AndroidManifest.xml')).Replace(
    '<manifest xmlns:android="http://schemas.android.com/apk/res/android"',
    '<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.lightningreturns.port"')
  [IO.File]::WriteAllText((Join-Path $w 'AndroidManifest.xml'), $manifest)
  $aapt2 = Join-Path $t.bt "aapt2$script:Exe"
  $log = Join-Path $script:AndroidLogDir 'apk.log'
  function Run([string]$exe, [string[]]$exeArgs, [string]$what) {
    $rc = Invoke-Logged $exe $exeArgs $log
    if ($rc -ne 0) { throw "$what failed; see $log" }
  }
  Run $aapt2 @('compile', '--dir', (Join-Path $proj 'res'), '-o', (Join-Path $w 'res\app.zip')) 'Compiling the app resources'
  Run $aapt2 @('compile', '--dir', (Join-Path $w 'sdlres'), '-o', (Join-Path $w 'res\sdl.zip')) 'Compiling the SDL resources'
  # The icon made from the game's title image (lr_icon), the same as the Windows one: an adaptive
  # icon (the portrait over a blurred copy of itself) and rounded squares for older launchers.
  $localRes = @()
  if ($iconDir -and (Test-Path -LiteralPath (Join-Path $iconDir 'lr_icon_full.png'))) {
    $lr = Join-Path $w 'localres'
    New-Item -ItemType Directory -Force -Path (Join-Path $lr 'drawable-nodpi'), (Join-Path $lr 'mipmap-anydpi-v26') | Out-Null
    Copy-Item -LiteralPath (Join-Path $iconDir 'lr_icon_full.png') -Destination (Join-Path $lr 'drawable-nodpi')
    foreach ($dpi in 'mdpi', 'hdpi', 'xhdpi', 'xxhdpi', 'xxxhdpi') {
      New-Item -ItemType Directory -Force -Path (Join-Path $lr "mipmap-$dpi") | Out-Null
      Copy-Item -LiteralPath (Join-Path $iconDir "ic_launcher_$dpi.png") -Destination (Join-Path $lr "mipmap-$dpi\ic_launcher.png")
    }
    [IO.File]::WriteAllText((Join-Path $lr 'mipmap-anydpi-v26\ic_launcher.xml'),
      "<?xml version=`"1.0`" encoding=`"utf-8`"?>`n<adaptive-icon xmlns:android=`"http://schemas.android.com/apk/res/android`">`n" +
      "    <background android:drawable=`"@drawable/lr_icon_full`" />`n    <foreground android:drawable=`"@android:color/transparent`" />`n</adaptive-icon>`n")
    Run $aapt2 @('compile', '--dir', $lr, '-o', (Join-Path $w 'res\local.zip')) 'Compiling the icon'
    $localRes = @('-R', (Join-Path $w 'res\local.zip'))
  }
  $version = Get-Date -Format 'yyyyMMdd'
  # The version code must grow with every build or Android refuses the update as a downgrade:
  # minutes since 2026-01-01 (UTC), above the step builds' codes (step number, under 1000).
  $code = [string][int](([DateTime]::UtcNow - [DateTime]::new(2026, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)).TotalMinutes + 1000)
  Run $aapt2 (@('link', '-o', (Join-Path $w 'base.apk'), '-I', $t.jar, '--manifest', (Join-Path $w 'AndroidManifest.xml'),
    '--min-sdk-version', '29', '--target-sdk-version', '35', '--version-code', $code, '--version-name', "builder-$version",
    '--java', (Join-Path $w 'gen'), '-R', (Join-Path $w 'res\sdl.zip'), '-R', (Join-Path $w 'res\app.zip')) + $localRes + @('--auto-add-overlay')) 'Linking the app resources'
  $sources = @(Get-ChildItem -LiteralPath (Join-Path $proj 'java'), (Join-Path $w 'java'), (Join-Path $w 'gen') -Recurse -Filter '*.java' | ForEach-Object { $_.FullName })
  [IO.File]::WriteAllLines((Join-Path $w 'sources.txt'), [string[]]($sources | ForEach-Object { '"' + ($_ -replace '\\', '/') + '"' }))
  $sep = $(if ($script:OnWindows) { ';' } else { ':' })
  Run $t.javac @('-nowarn', '-Xlint:-options', '-source', '8', '-target', '8', '-encoding', 'UTF-8',
    '-bootclasspath', "$($t.jar)$sep$(Join-Path $t.bt 'core-lambda-stubs.jar')", '-d', (Join-Path $w 'classes'), "@$(Join-Path $w 'sources.txt')") 'Compiling the Java code'
  $classes = @(Get-ChildItem -LiteralPath (Join-Path $w 'classes') -Recurse -Filter '*.class' | ForEach-Object { $_.FullName })
  Run $t.java (@('-cp', (Join-Path $t.bt 'lib\d8.jar'), 'com.android.tools.r8.D8', '--release', '--min-api', '29', '--lib', $t.jar,
    '--output', (Join-Path $w 'dex')) + $classes) 'Converting the Java code (d8)'
  # Add classes.dex and the native libraries (stored, so zipalign can page-align them).
  Copy-Item -LiteralPath (Join-Path $w 'base.apk') -Destination (Join-Path $w 'unaligned.apk')
  Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
  $zip = [IO.Compression.ZipFile]::Open((Join-Path $w 'unaligned.apk'), 'Update')
  try {
    foreach ($dex in Get-ChildItem -LiteralPath (Join-Path $w 'dex') -Filter 'classes*.dex') {
      [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $dex.FullName, $dex.Name, [IO.Compression.CompressionLevel]::Optimal)
    }
    foreach ($so in Get-ChildItem -LiteralPath (Join-Path $libRoot 'arm64-v8a') -Filter '*.so') {
      [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $so.FullName, "lib/arm64-v8a/$($so.Name)", [IO.Compression.CompressionLevel]::NoCompression)
    }
  } finally { $zip.Dispose() }
  Run (Join-Path $t.bt "zipalign$script:Exe") @('-f', '-P', '16', '4', (Join-Path $w 'unaligned.apk'), (Join-Path $w 'aligned.apk')) 'Aligning the APK'
  if (-not (Test-Path -LiteralPath $a.keystore)) {
    Run $t.keytool @('-genkeypair', '-keystore', $a.keystore, '-storepass', 'android', '-keypass', 'android', '-alias', 'androiddebugkey',
      '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000', '-dname', 'CN=Android Debug,O=Android,C=US') 'Making the signing key'
  }
  Run $t.java @('-jar', (Join-Path $t.bt 'lib\apksigner.jar'), 'sign', '--ks', $a.keystore, '--ks-pass', 'pass:android', '--key-pass', 'pass:android',
    '--ks-key-alias', 'androiddebugkey', '--min-sdk-version', '29', '--v1-signing-enabled', 'false', '--v2-signing-enabled', 'true',
    '--v3-signing-enabled', 'true', '--out', $a.apk, (Join-Path $w 'aligned.apk')) 'Signing the APK'
  Run $t.java @('-jar', (Join-Path $t.bt 'lib\apksigner.jar'), 'verify', $a.apk) 'Checking the APK signature'
}

# Install on the phone: the app, the 360 game files, the PS3 button / UI set, PS3 saves, and the
# PC's user data (saves, DLC, settings) the first time.
function Step-Phone($state, $p) {
  $a = Get-AndroidPaths $p
  if (-not $script:AT) { $script:AndroidLogDir = Join-Path $p.logs 'android'; $script:AT = Get-AndroidTools $p }
  $adb = $script:AT.adb
  Say ''
  Say 'Connect the phone by USB with USB debugging on (Settings > Developer options), unlock it, and allow this PC.'
  while ($true) {
    $a1 = (Read-Host 'Press Enter when it is connected (or type s to skip installing - the APK stays in the output folder)').Trim().ToLowerInvariant()
    if ($a1 -eq 's') { Say "  skipped; the APK is $($a.apk)"; return }
    $devices = @(& $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match "`tdevice$" })
    if ($devices.Count -eq 1) { break }
    if ($devices.Count -gt 1) { Say 'More than one phone is connected; connect only one.' 'Yellow' } else { Say 'No phone found yet (is USB debugging on, and did you allow this PC on the phone?).' 'Yellow' }
  }
  & $adb shell am force-stop com.lightningreturns.port 2>$null | Out-Null
  $log = Join-Path $p.logs 'phone.log'
  Say '  installing the app...'
  $rc = Invoke-Logged $adb @('install', '-r', $a.apk) $log
  if ($rc -ne 0) {
    $why = Get-Content -LiteralPath $log -Raw
    if ($why -match 'INSTALL_FAILED_UPDATE_INCOMPATIBLE|INSTALL_FAILED_VERSION_DOWNGRADE') {
      if ($why -match 'DOWNGRADE') { Say 'The phone has a newer-numbered build of the app; it has to be removed first (the game files and saves on the phone stay).' 'Yellow' }
      else { Say 'The phone has the app signed by another PC; it has to be removed first (the game files and saves on the phone stay).' 'Yellow' }
      if (-not (Ask-YesNo 'Remove the old app and install this one?')) { throw 'Not installed.' }
      $null = Invoke-Logged $adb @('uninstall', 'com.lightningreturns.port') $log
      $rc = Invoke-Logged $adb @('install', $a.apk) $log
      if ($rc -ne 0) { throw "adb install failed; see $log" }
    } else { throw "adb install failed; see $log" }
  }
  $phoneGame = '/sdcard/LightningReturns'
  $phoneUser = '/sdcard/LightningReturns-userdata'
  # Copies to the phone (only changed files), with a "still working" line every 15 s.
  function Push([string]$from, [string]$to, [string]$what) {
    $rc = Invoke-Logged $adb @('push', '--sync', $from, $to) (Join-Path $p.logs 'phone_push.log')
    if ($rc -ne 0) { throw "Copying $what to the phone failed; see $(Join-Path $p.logs 'phone_push.log')" }
  }
  Say '  copying the 360 game files (about 7 GB the first time, 10-20 minutes; later only changed files)...'
  & $adb shell mkdir -p $phoneGame | Out-Null
  Push "$($p.game)$([IO.Path]::DirectorySeparatorChar)." "$phoneGame/" 'the game files'
  & $adb shell mkdir -p "$phoneUser/textures" | Out-Null
  $ps3ui = Join-Path $p.textures 'ps3ui'
  if (Test-Path -LiteralPath $ps3ui) { Push $ps3ui "$phoneUser/textures/" 'the PS3 buttons' }
  # Japanese voices and text (Settings > Voice Language / Text Language), if the Steam step made them.
  $jp = Join-Path $p.install 'jpvoice'
  if (Test-Path -LiteralPath (Join-Path $jp 'filelist_patch_u.x360.bin')) {
    Say '  copying the Japanese voices / text...'
    Push $jp "$phoneUser/" 'the Japanese voices'
  }
  # DLC packs: every time (only changed files); the phone reads them from the same place.
  $dlc = Join-Path $p.userData '0000000000000000\5351081B'
  if (Test-Path -LiteralPath (Join-Path $dlc '00000002')) {
    Say '  copying the DLC...'
    & $adb shell mkdir -p "$phoneUser/0000000000000000/5351081B/Headers" | Out-Null
    Push (Join-Path $dlc '00000002') "$phoneUser/0000000000000000/5351081B/" 'the DLC'
    $hd = Join-Path $dlc 'Headers\00000002'
    if (Test-Path -LiteralPath $hd) { Push $hd "$phoneUser/0000000000000000/5351081B/Headers/" 'the DLC' }
  }
  # Saves, DLC and settings from this PC, the first time only (the phone keeps its own after that).
  $marker = (& $adb shell "ls $phoneUser/B13EBABEBABEBABE 2>/dev/null") -join ''
  if (-not $marker -and (Test-Path -LiteralPath $p.userData)) {
    foreach ($sub in 'B13EBABEBABEBABE', '0000000000000000', 'ps3_saves', 'steam_saves') {
      $src = Join-Path $p.userData $sub
      if (Test-Path -LiteralPath $src) { Push $src "$phoneUser/" 'the saves' }
    }
  }
  Say '  installed. Start "Lightning Returns" on the phone; allow file access when it asks.' 'Green'
}
