# Checks the Windows zip on a real Windows machine (the automatic builds run it on GitHub's
# Windows): the game starts and says its version, and play.bat does on Windows' own cmd what it
# was tested to do under Wine:
#  1. with no game anywhere it says where the data goes, and stops (exit 1);
#  2. it finds UFO in Steam's own library (Steam's folder from the registry, with a space and
#     brackets in it as "Program Files (x86)" has) and TFTD in a second library on another disk
#     (from Steam's libraryfolders.vdf), links both as junctions, writes the starting options
#     and starts the game;
#  3. when the data moves (Steam's UFO gone, a UFO folder next to the game's folder instead),
#     the old link goes and the new place is found.
# The games' data is a stand-in (their four folders, empty): the game then stops at loading it,
# which is as far as a machine without the real data gets. The original folders are only read.
# Usage: windows_zip_check.ps1 -Zip OpenXcom3D-<version>-windows-x86_64.zip
param([Parameter(Mandatory = $true)][string]$Zip)
$ErrorActionPreference = 'Stop'
$script:failures = 0

# One check's result, and a count of the failed ones
function Check([bool]$ok, [string]$what) {
	if ($ok) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:failures++ }
}

# play.bat run as a double-click runs it (from its folder, its pause answered at once): its
# output and its exit code. It's started as Explorer starts it (ShellExecute, its own hidden
# console), with its output in a file: started from here, cmd and the game it starts would
# inherit this script's output pipe, and the wait for play.bat would last as long as the game.
function Run-PlayBat([string]$folder) {
	$outputFile = Join-Path $env:RUNNER_TEMP 'play-bat.txt'
	Remove-Item $outputFile -ErrorAction SilentlyContinue
	$start = New-Object System.Diagnostics.ProcessStartInfo 'cmd.exe', "/d /c `"play.bat <NUL >`"$outputFile`" 2>&1`""
	$start.UseShellExecute = $true
	$start.WorkingDirectory = $folder
	$start.WindowStyle = 'Hidden'
	$process = [System.Diagnostics.Process]::Start($start)
	# (two minutes at most: a play.bat that hangs fails its check with what it wrote so far)
	$ended = $process.WaitForExit(120000)
	if (-not $ended) { $process.Kill($true) }
	$output = Get-Content $outputFile -Raw -ErrorAction SilentlyContinue
	Write-Host $output
	Check $ended 'play.bat ended by itself (within two minutes)'
	return @{ Output = "$output"; Code = $(if ($ended) { $process.ExitCode } else { -1 }) }
}

# A stand-in for one game's data: its four folders, and a file to see it's left as it was
function New-GameData([string]$folder) {
	foreach ($part in 'GEODATA', 'GEOGRAPH', 'MAPS', 'UFOGRAPH') { New-Item -ItemType Directory -Force -Path (Join-Path $folder $part) | Out-Null }
	Set-Content -Path (Join-Path $folder 'GEODATA\stand-in.txt') -Value 'not the real data'
}

# Stops the game play.bat started (it waits at the stand-in data's error)
function Stop-Game {
	Start-Sleep -Seconds 8
	Get-Process -Name openxcom -ErrorAction SilentlyContinue | Stop-Process -Force
	Start-Sleep -Seconds 1
}

# The zip, unpacked where many players' folders are: a path with a space and brackets in it
$work = Join-Path $env:RUNNER_TEMP 'Games (zip check)'
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
Expand-Archive -Path $Zip -DestinationPath $work
$game = (Get-ChildItem -Path $work -Directory | Select-Object -First 1).FullName
$profileDir = Join-Path $game 'user'
Write-Host "the game: $game"
Check ((Get-Content (Join-Path $game 'play.bat') -Raw) -match "`r`n") 'play.bat has Windows line ends'

# The game starts on this Windows (its DLLs load) and says its version. (SDL 1.2's start on
# Windows sends a game's text to bin\stdout.txt; SDL_STDIO_REDIRECT=0 keeps it on the output,
# for this check only.)
$versionFile = Join-Path $env:RUNNER_TEMP 'version.txt'
$env:SDL_STDIO_REDIRECT = '0'
$process = Start-Process -FilePath (Join-Path $game 'bin\openxcom.exe') -ArgumentList '-version' -RedirectStandardOutput $versionFile -NoNewWindow -Wait -PassThru
Remove-Item Env:SDL_STDIO_REDIRECT
$version = "$(Get-Content $versionFile -Raw -ErrorAction SilentlyContinue)".Trim()
Check ($process.ExitCode -eq 0 -and $version -ne '') "openxcom.exe -version ran (exit $($process.ExitCode)): $version"

# 1. No game anywhere: it says where the data goes, and stops
$result = Run-PlayBat $game
Check ($result.Code -eq 1 -and $result.Output -match 'needs the data of the original game') 'no game data: play.bat says where it goes and stops'

# 2. Steam: UFO in its own library, TFTD in a second one on another disk
$steam = Join-Path $env:RUNNER_TEMP 'Program Files (x86)\Steam'
$steamUfo = Join-Path $steam 'steamapps\common\XCom UFO Defense\XCOM'
# (GitHub's Windows puts the temporary folder on D:, so the second library goes on C:)
$library2 = 'C:\SteamLibrary (check)'
$steamTftd = Join-Path $library2 'steamapps\common\X-COM Terror from the Deep\TFD'
New-GameData $steamUfo
New-GameData $steamTftd
# (Steam writes its list with doubled backslashes, in UTF-8 without a BOM)
$vdf = "`"libraryfolders`"`n{`n`t`"0`"`n`t{`n`t`t`"path`"`t`t`"$($steam -replace '\\', '\\')`"`n`t}`n`t`"1`"`n`t{`n`t`t`"path`"`t`t`"$($library2 -replace '\\', '\\')`"`n`t}`n}`n"
[System.IO.File]::WriteAllText((Join-Path $steam 'steamapps\libraryfolders.vdf'), $vdf, (New-Object System.Text.UTF8Encoding $false))
# (and its folder in the registry as Steam writes it: forward slashes, lower case)
New-Item -Path 'HKCU:\Software\Valve\Steam' -Force | Out-Null
Set-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -Value (($steam -replace '\\', '/').ToLower())
$result = Run-PlayBat $game
Check ($result.Code -eq 0) "Steam: play.bat went on to start the game (exit $($result.Code))"
Check ($result.Output -match 'using the UFO data in') 'Steam: UFO found in Steam''s own library'
Check ($result.Output -match 'using the TFTD data in') 'Steam: TFTD found in the second library, on another disk'
$ufoLink = Get-Item (Join-Path $profileDir 'UFO') -ErrorAction SilentlyContinue
$tftdLink = Get-Item (Join-Path $profileDir 'TFTD') -ErrorAction SilentlyContinue
Check ($ufoLink -and $ufoLink.LinkType -eq 'Junction' -and "$($ufoLink.Target)" -eq $steamUfo) "user\UFO is a junction to Steam's UFO ($($ufoLink.Target))"
Check ($tftdLink -and $tftdLink.LinkType -eq 'Junction' -and "$($tftdLink.Target)" -eq $steamTftd) "user\TFTD is a junction to the second library's TFTD ($($tftdLink.Target))"
$options = Get-Content (Join-Path $profileDir 'options.cfg') -Raw -ErrorAction SilentlyContinue
Check ($options -match "active: true\r?\n\s+id: xcom1" -and $options -match "active: true\r?\n\s+id: OpenXcom3D") 'the starting options: UFO and the OpenXcom3D mod on'
Check ($null -ne (Get-Process -Name openxcom -ErrorAction SilentlyContinue)) 'the game is running'
Start-Sleep -Seconds 6
Check (Test-Path (Join-Path $profileDir 'openxcom.log')) 'the game wrote its log in user\'
Stop-Game

# 3. The data moved: Steam's UFO is gone, a UFO folder next to the game's folder instead
Remove-Item -Recurse -Force $steamUfo
$nextToUfo = Join-Path $work 'UFO'
New-GameData $nextToUfo
$result = Run-PlayBat $game
$ufoLink = Get-Item (Join-Path $profileDir 'UFO') -ErrorAction SilentlyContinue
Check ($result.Code -eq 0 -and $ufoLink -and "$($ufoLink.Target)" -eq $nextToUfo) "moved: user\UFO now links the UFO folder next to the game ($($ufoLink.Target))"
Stop-Game

# The originals were only read
Check ((Test-Path (Join-Path $steamTftd 'GEODATA\stand-in.txt')) -and (Test-Path (Join-Path $nextToUfo 'GEODATA\stand-in.txt'))) 'the games'' folders are as they were'

if ($script:failures -gt 0) { Write-Host "$($script:failures) check(s) failed"; exit 1 }
Write-Host 'all checks passed'
