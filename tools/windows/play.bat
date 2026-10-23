@echo off
rem Starts OpenXcom3D on Windows. It plays with the data and the profile in this folder (user\).
rem The first time, it links the original games' data into the profile: a UFO or TFTD folder in
rem this folder or next to it, or X-COM: UFO Defense and Terror From The Deep from a Steam library.
rem Extra arguments are passed on, e.g.  play.bat -load mysave.sav
rem (make_release_windows.sh gives it Windows line ends; paths are always in quotes, since
rem Steam's own folder has brackets in it)
setlocal EnableExtensions DisableDelayedExpansion

rem Steam's list of its libraries is UTF-8: read as such (the console gets its own code page
rem back at the end)
set "OLD_CP="
for /f "tokens=2 delims=:." %%c in ('chcp') do set "OLD_CP=%%c"
chcp 65001 > nul

rem This folder, the profile in it, and the folder it's in
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
set "PROFILE=%HERE%\user"
for %%I in ("%HERE%\..") do set "NEXT_TO=%%~fI"
set "STEAM="

rem The profile must be writable (not so in Program Files)
if not exist "%PROFILE%\" mkdir "%PROFILE%" > nul 2>&1
copy /y nul "%PROFILE%\write-test.tmp" > nul 2>&1 || goto not_writable
del "%PROFILE%\write-test.tmp" > nul 2>&1

rem The games' data, and a clear word when there's none
call :link_game UFO "XCom UFO Defense\XCOM"
call :link_game TFTD "X-COM Terror from the Deep\TFD"
if exist "%PROFILE%\UFO\GEODATA\" goto have_game
if exist "%PROFILE%\TFTD\GEODATA\" goto have_game
echo.
echo OpenXcom3D needs the data of the original game: X-COM: UFO Defense (UFO: Enemy Unknown)
echo and/or X-COM: Terror From The Deep. Install it on Steam (it is then found by itself), or
echo copy the game's folder (the one with GEODATA, GEOGRAPH, MAPS and UFOGRAPH in it) to
echo   "%PROFILE%\UFO"   (or TFTD)
echo and start OpenXcom3D again.
echo.
pause
goto fail

:have_game
rem The first time: the starting options
if not exist "%PROFILE%\options.cfg" call :write_options
if defined OLD_CP chcp %OLD_CP% > nul 2>&1
start "" /D "%HERE%" "%HERE%\bin\openxcom.exe" -data "%HERE%\data/" -user "%PROFILE%/" -cfg "%PROFILE%/" %*
endlocal
exit /b 0

:not_writable
echo.
echo OpenXcom3D can't write to its folder:
echo   "%HERE%"
echo Move the whole folder somewhere of yours (Documents, the Desktop or C:\Games, not
echo Program Files) and start it there.
echo.
pause
goto fail

:fail
if defined OLD_CP chcp %OLD_CP% > nul 2>&1
endlocal
exit /b 1


rem Links one game's data into the profile unless it's there: the game's name (UFO or TFTD)
rem and its folder in a Steam library. A link left pointing nowhere (the data moved) goes first
rem (rmdir takes only an empty folder or a link, never the data a link points to).
:link_game
if exist "%PROFILE%\%~1\GEODATA\" exit /b 0
if exist "%PROFILE%\%~1" rmdir "%PROFILE%\%~1" > nul 2>&1
call :try_folder %1 "%HERE%\%~1" && exit /b 0
call :try_folder %1 "%NEXT_TO%\%~1" && exit /b 0
call :find_steam
if not defined STEAM exit /b 1
call :try_folder %1 "%STEAM%\steamapps\common\%~2" && exit /b 0
if not exist "%STEAM%\steamapps\libraryfolders.vdf" exit /b 1
rem (the other libraries, e.g. on another disk: the "path" lines, as  "path"  "D:\\SteamLibrary";
rem the first word with its quotes, then the rest of the line, the folder in its quotes)
for /f "usebackq tokens=1,*" %%a in ("%STEAM%\steamapps\libraryfolders.vdf") do (
	if /i %%a=="path" call :try_library %1 %%b %2 && exit /b 0
)
exit /b 1

rem A Steam library from its list: the game's name, the library's folder (with its backslashes
rem doubled, as the list writes them) and the game's folder in it.
:try_library
set "LIBRARY=%~2"
set "LIBRARY=%LIBRARY:\\=\%"
call :try_folder %1 "%LIBRARY%\steamapps\common\%~3" && exit /b 0
exit /b 1

rem Steam's own folder (STEAM), from the registry or where it installs by default.
:find_steam
if defined STEAM exit /b 0
for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| find /i "SteamPath"') do set "STEAM=%%b"
if not defined STEAM for /f "tokens=2,*" %%a in ('reg query "HKLM\SOFTWARE\WOW6432Node\Valve\Steam" /v InstallPath 2^>nul ^| find /i "InstallPath"') do set "STEAM=%%b"
if not defined STEAM if exist "%ProgramFiles(x86)%\Steam\steamapps\" set "STEAM=%ProgramFiles(x86)%\Steam"
if defined STEAM set "STEAM=%STEAM:/=\%"
exit /b 0

rem Links a game's data into the profile if the folder holds it (both games have these folders):
rem the game's name and the folder. A junction needs no administrator; where there can be none
rem (a FAT32 or exFAT disk, a network drive), the data is copied instead.
:try_folder
if not exist "%~2\GEODATA\" exit /b 1
if not exist "%~2\GEOGRAPH\" exit /b 1
if not exist "%~2\MAPS\" exit /b 1
if not exist "%~2\UFOGRAPH\" exit /b 1
mklink /J "%PROFILE%\%~1" "%~2" > nul 2>&1 && goto try_folder_done
xcopy "%~2" "%PROFILE%\%~1" /E /I /Q /Y > nul 2>&1 || exit /b 1
:try_folder_done
echo OpenXcom3D: using the %~1 data in "%~2"
exit /b 0

rem The starting options: the game that's there (UFO if both), and a 1280x800 window where the
rem screen has room for it, else the full screen (the game counts real pixels, whatever the
rem display's scaling).
:write_options
set "UFO_ACTIVE=true"
set "TFTD_ACTIVE=false"
if not exist "%PROFILE%\UFO\GEODATA\" set "UFO_ACTIVE=false"
if not exist "%PROFILE%\UFO\GEODATA\" set "TFTD_ACTIVE=true"
set "FIT="
for /f %%f in ('powershell -NoProfile -NonInteractive -Command "$v = Get-CimInstance Win32_VideoController | Where-Object CurrentVerticalResolution | Select-Object -First 1; if ($v.CurrentHorizontalResolution -ge 1300 -and $v.CurrentVerticalResolution -ge 900) {echo window} else {echo full}" 2^>nul') do set "FIT=%%f"
set "FULLSCREEN=true"
if "%FIT%"=="window" set "FULLSCREEN=false"
(
	echo mods:
	echo   - active: %UFO_ACTIVE%
	echo     id: xcom1
	echo   - active: %TFTD_ACTIVE%
	echo     id: xcom2
	echo   - active: true
	echo     id: OpenXcom3D
	echo options:
	echo   displayWidth: 1280
	echo   displayHeight: 800
	echo   fullscreen: %FULLSCREEN%
	echo   keepAspectRatio: true
	echo   battlescapeScale: 4
	echo   geoscapeScale: 4
	echo   FPS: 60
) > "%PROFILE%\options.cfg"
exit /b 0
