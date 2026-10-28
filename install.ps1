# asterism installer for Windows.
#
#   irm https://raw.githubusercontent.com/gslf/asterism/main/install.ps1 | iex
#   .\install.ps1                  inside a clone: builds this checkout
#
# It installs asterism.exe (a release archive, or a build from source) under
# %LOCALAPPDATA%\Programs\asterism, adds it to your PATH, then runs
# "asterism setup", which picks the models for this machine, asks for the
# Telegram bot token, pairs your account and starts the agent at every login.
#
# With options, through a script block:
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/gslf/asterism/main/install.ps1))) -Uninstall
#
#   -FromSource   build from source (needs git, CMake and Visual Studio C++)
#   -Prefix DIR   install directory
#   -Root DIR     engine root passed to setup (default %USERPROFILE%\asterism)
#   -NoSetup      install only
#   -Yes          accept every default without asking
#   -Uninstall    stop the agent, remove the program (keeps your data)
#   -Purge        with -Uninstall: also delete the engine root
#
# $env:ASTERISM_REPO = "owner/name" selects another GitHub repository.
param(
  [switch]$FromSource,
  [string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\asterism'),
  [string]$Root = '',
  [switch]$NoSetup,
  [switch]$Yes,
  [switch]$Uninstall,
  [switch]$Purge
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is slow with it
$Repo = if ($env:ASTERISM_REPO) { $env:ASTERISM_REPO } else { 'gslf/asterism' }
$Bin = Join-Path $Prefix 'bin'
$Exe = Join-Path $Bin 'asterism.exe'

function Confirm-Step([string]$Question, [bool]$Default = $true) {
  if ($Yes -or -not [Environment]::UserInteractive) { return $Default }
  $hint = if ($Default) { '[Y/n]' } else { '[y/N]' }
  $answer = Read-Host "$Question $hint"
  if ([string]::IsNullOrWhiteSpace($answer)) { return $Default }
  return $answer -match '^(y|yes)$'
}

function Test-Command([string]$Name) {
  return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Set-UserPath([string]$Dir, [bool]$Add) {
  $current = [Environment]::GetEnvironmentVariable('Path', 'User')
  $parts = @($current -split ';' | Where-Object { $_ -and ($_.TrimEnd('\') -ne $Dir.TrimEnd('\')) })
  if ($Add) { $parts += $Dir }
  [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'User')
  if ($Add -and ($env:Path -split ';') -notcontains $Dir) { $env:Path = "$env:Path;$Dir" }
}

$setupRoot = @()
if ($Root) { $setupRoot = @('--root', $Root) }

# -- uninstall ------------------------------------------------------------
if ($Uninstall) {
  if (Test-Path $Exe) { & $Exe setup --remove-service @setupRoot }
  if (Test-Path $Prefix) { Remove-Item -Recurse -Force $Prefix }
  Set-UserPath $Bin $false
  Write-Host "asterism removed from $Prefix"
  $data = if ($Root) { $Root } else { Join-Path $env:USERPROFILE 'asterism' }
  if ($Purge -and (Test-Path $data)) {
    $answer = Read-Host "Delete $data with its memory, sessions, models and secrets? Type 'delete'"
    if ($answer -eq 'delete') { Remove-Item -Recurse -Force $data; Write-Host "$data deleted" }
  } else {
    Write-Host "Your data stays in $data"
  }
  return
}

# -- platform -------------------------------------------------------------
$arch = switch ($env:PROCESSOR_ARCHITECTURE) {
  'AMD64' { 'x86_64' }
  'ARM64' { 'arm64' }
  default { $env:PROCESSOR_ARCHITECTURE.ToLower() }
}
$platform = "windows-$arch"

# Run as .\install.ps1 inside a clone: that checkout is the source.
$here = if ($PSScriptRoot) { $PSScriptRoot } else { '' }
if ($here -and (Test-Path (Join-Path $here 'CMakeLists.txt')) -and (Test-Path (Join-Path $here 'asngn'))) {
  $FromSource = $true
} else {
  $here = ''
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("asterism-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null

function Install-Release {
  $asset = "asterism-$platform.zip"
  $url = "https://github.com/$Repo/releases/latest/download/$asset"
  $zip = Join-Path $tmp $asset
  Write-Host "Downloading $asset"
  try { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip } catch { return $false }
  Invoke-WebRequest -UseBasicParsing -Uri "$url.sha256" -OutFile "$zip.sha256"
  $want = ((Get-Content "$zip.sha256" -Raw).Trim() -split '\s+')[0].ToLower()
  $got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
  if ($want -ne $got) { throw "$asset does not match its checksum" }
  $x = Join-Path $tmp 'x'
  Expand-Archive -Path $zip -DestinationPath $x
  $top = Get-ChildItem $x -Directory | Select-Object -First 1
  if (-not (Test-Path (Join-Path $top.FullName 'bin\asterism.exe'))) { throw "$asset holds no bin\asterism.exe" }
  New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
  Copy-Item -Recurse -Force (Join-Path $top.FullName '*') $Prefix
  return $true
}

function Install-Source {
  foreach ($tool in 'git', 'cmake') {
    if (-not (Test-Command $tool)) {
      $id = if ($tool -eq 'git') { 'Git.Git' } else { 'Kitware.CMake' }
      if ((Test-Command winget) -and (Confirm-Step "Install $tool with: winget install $id ?")) {
        winget install --id $id -e --source winget
        $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
                    [Environment]::GetEnvironmentVariable('Path', 'User')
      } else {
        throw "$tool is needed to build asterism"
      }
    }
  }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not ((Test-Path $vswhere) -and (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath))) {
    $id = 'Microsoft.VisualStudio.2022.BuildTools'
    $cmd = "winget install --id $id -e --override `"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`""
    Write-Host "Building needs the Visual Studio C++ build tools: $cmd"
    if ((Test-Command winget) -and (Confirm-Step 'Install them now?')) {
      winget install --id $id -e --override '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
    } else {
      throw 'install the Visual Studio C++ build tools, then run this again'
    }
  }
  if ($here) {
    $src = $here
  } else {
    $src = Join-Path $env:LOCALAPPDATA 'asterism\src'
    if (Test-Path (Join-Path $src '.git')) {
      git -C $src pull --ff-only
    } else {
      git clone "https://github.com/$Repo.git" $src
    }
  }
  if (Test-Path (Join-Path $src '.git')) { git -C $src submodule update --init --recursive }
  $flags = @('-DASTERISM_BUILD_TESTS=OFF')
  if (Test-Command nvcc) { Write-Host 'CUDA found: building with GPU support'; $flags += '-DGGML_CUDA=ON' }
  $build = Join-Path $src 'build-install'
  Write-Host 'Building asterism (a few minutes)'
  cmake -S $src -B $build @flags
  if ($LASTEXITCODE) { throw 'cmake configure failed' }
  cmake --build $build --config Release -j
  if ($LASTEXITCODE) { throw 'build failed' }
  cmake --install $build --config Release --prefix $Prefix
  if ($LASTEXITCODE) { throw 'install failed' }
}

# An update replaces the program, which the running agent locks: stop it
# first and start it again after.
$wasRunning = $false
if (Test-Path $Exe) {
  $wasRunning = [bool]((& $Exe status @setupRoot) -match 'is running')
  if ($wasRunning) { & $Exe stop @setupRoot | Out-Null }
}

try {
  if (-not $FromSource) {
    if (-not (Install-Release)) {
      Write-Host "No release archive for ${platform}: building from source"
      Install-Source
    }
  } else {
    Install-Source
  }
} finally {
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

Write-Host "Installed: $(& $Exe --version) in $Exe"
if (($env:Path -split ';') -notcontains $Bin) {
  Set-UserPath $Bin $true
  Write-Host "Added $Bin to your PATH (new terminals see it)"
}

if ($NoSetup) {
  Write-Host 'Next: asterism setup'
} elseif ($Yes) {
  & $Exe setup --yes @setupRoot
} else {
  & $Exe setup @setupRoot
}
if ($wasRunning -and -not ((& $Exe status @setupRoot) -match 'is running')) { & $Exe start @setupRoot }
