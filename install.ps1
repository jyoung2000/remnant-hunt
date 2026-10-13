# Remnant Hunt installer: finds Monster Hunter: World in your Steam libraries and installs the mod into it.
#
#   One line, in PowerShell:
#     irm https://raw.githubusercontent.com/jyoung2000/remnant-hunt/main/install.ps1 | iex
#   From a release zip you downloaded:
#     .\install.ps1 -Zip .\remnant_hunt-0.4.0.zip
#   Options: -GameDir "D:\Games\Monster Hunter World"   -Force (replace another mod loader's dinput8.dll, kept as .bak)
#   Uninstall: .\install.ps1 -Uninstall
#
# Only the mod's own files are written: dinput8.dll and the remnant_hunt folder. Your settings (remnant_hunt.ini)
# and your Remnant progress (%LOCALAPPDATA%\RemnantHunt) are kept.
param([string]$Zip = "", [string]$GameDir = "", [string]$Repo = "jyoung2000/remnant-hunt", [switch]$Force,
      [switch]$Uninstall)
$ErrorActionPreference = "Stop"

function Find-SteamApp([string]$folder) {
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { $steam = "${env:ProgramFiles(x86)}\Steam" }
    $libs = @($steam)
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        $libs += Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' |
            ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
    }
    foreach ($l in $libs) {
        $p = Join-Path $l "steamapps\common\$folder"
        if (Test-Path $p) { return $p }
    }
    return $null
}

function Test-OurLoader([string]$dll) {
    $bytes = [IO.File]::ReadAllBytes($dll)
    return [Text.Encoding]::ASCII.GetString($bytes).Contains("remnant_hunt")
}

if (-not $GameDir) { $GameDir = Find-SteamApp "Monster Hunter World" }
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir "MonsterHunterWorld.exe"))) {
    throw "Monster Hunter: World was not found. Run again with -GameDir 'D:\path\to\Monster Hunter World'."
}
if (Get-Process MonsterHunterWorld -ErrorAction SilentlyContinue) { throw "Close Monster Hunter: World first." }
$loader = Join-Path $GameDir "dinput8.dll"

if ($Uninstall) {
    if ((Test-Path $loader) -and (Test-OurLoader $loader)) { Remove-Item $loader }
    if (Test-Path "$loader.bak") { Move-Item "$loader.bak" $loader }
    $mod = Join-Path $GameDir "remnant_hunt"
    if (Test-Path $mod) { Remove-Item $mod -Recurse }
    Write-Host "Remnant Hunt removed from $GameDir (your progress in %LOCALAPPDATA%\RemnantHunt is kept)."
    return
}

if (-not (Find-SteamApp "Remnant2")) {
    Write-Warning ("Remnant II was not found in your Steam libraries. The mod reads Remnant II's models, sounds and " +
                   "text from your own install: set remnant2= under [paths] in remnant_hunt\remnant_hunt.ini.")
}

# Another mod loader (Stracker's Loader also uses dinput8.dll): never replace it silently.
if ((Test-Path $loader) -and -not (Test-OurLoader $loader)) {
    if (-not $Force) {
        throw ("$loader belongs to another mod loader. Remnant Hunt needs that file name too. Run again with -Force " +
               "to keep yours as dinput8.dll.bak and install Remnant Hunt's.")
    }
    Copy-Item $loader "$loader.bak" -Force
}

if (-not $Zip) {
    $rel = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ "User-Agent" = "remnant-hunt" }
    $asset = $rel.assets | Where-Object { $_.name -like "remnant_hunt-*.zip" } | Select-Object -First 1
    if (-not $asset) { throw "The latest release of $Repo has no remnant_hunt zip." }
    $Zip = Join-Path $env:TEMP $asset.name
    Write-Host "Downloading $($asset.name) ..."
    Invoke-WebRequest $asset.browser_download_url -OutFile $Zip -Headers @{ "User-Agent" = "remnant-hunt" }
}
Expand-Archive -Path $Zip -DestinationPath $GameDir -Force
Write-Host "Remnant Hunt installed into $GameDir. Start Monster Hunter: World from Steam."
