@echo off
setlocal EnableDelayedExpansion
set "KURONO_DIR=%~dp0"
set "SIM=%KURONO_DIR%kurono_os_sim.py"
set "LOG_DIR=D:\Kurono\KuronoOS\Build_Files\logs"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"
where python >nul 2>&1 && set "PY=python" || set "PY=python"
if "%~1"=="" goto usage
if /I "%~1"=="start" goto start
if /I "%~1"=="run" goto run
if /I "%~1"=="feed" goto feed
if /I "%~1"=="test" goto test
if /I "%~1"=="alias-add" goto alias_add
if /I "%~1"=="alias-list" goto alias_list
if /I "%~1"=="alias-run" goto alias_run
if /I "%~1"=="alias-remove" goto alias_remove
if /I "%~1"=="logs" goto show_logs
if /I "%~1"=="help" goto usage
goto usage

:start
%PY% "%SIM%"
goto end

:run
shift
set "LINE="
:run_collect
if "%~1"=="" goto run_go
set "LINE=!LINE! %~1"
shift
goto run_collect
:run_go
if "!LINE!"=="" goto usage
set "TMP=%TEMP%\kurono_cmd_input.txt"
>"!TMP!" echo(!LINE:~1!
>>"!TMP!" echo(exit
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-Content \"$env:TEMP\\kurono_cmd_input.txt\" ^| %PY% \"%SIM%\"" > "%LOG_DIR%\last.log"
type "%LOG_DIR%\last.log"
goto end

:feed
set "FILE=%~2"
if "%FILE%"=="" goto usage
if not exist "%FILE%" echo File not found: %FILE% & goto end
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-Content \"%FILE%\" ^| %PY% \"%SIM%\"" > "%LOG_DIR%\last.log"
type "%LOG_DIR%\last.log"
goto end

:test
%PY% "%SIM%" --test > "%LOG_DIR%\last.log"
type "%LOG_DIR%\last.log"
goto end

:alias_add
set "ALIASES=D:\Important\Kurono\Commands\aliases.txt"
for /f %%I in ('powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path 'D:\\Important\\Kurono\\Commands' ^| Out-Null; Write-Output 1"') do set DUMMY=%%I
set "NAME=%~2"
shift
shift
set "LINE="
:alias_collect
if "%~1"=="" goto alias_go
set "LINE=!LINE! %~1"
shift
goto alias_collect
:alias_go
if "%NAME%"=="" goto usage
if "!LINE!"=="" goto usage
set "CMD=!LINE:~1!"
>>"%ALIASES%" echo %NAME%=%CMD%
echo Alias added: %NAME%
goto end

:alias_list
set "ALIASES=D:\Important\Kurono\Commands\aliases.txt"
if not exist "%ALIASES%" echo No aliases defined.& goto end
type "%ALIASES%"
goto end

:alias_run
set "ALIASES=D:\Important\Kurono\Commands\aliases.txt"
set "NAME=%~2"
if "%NAME%"=="" goto usage
if not exist "%ALIASES%" echo No aliases defined.& goto end
set "CMD="
for /f "usebackq tokens=1* delims==" %%A in ("%ALIASES%") do if /I "%%A"=="%NAME%" set "CMD=%%B"
if "%CMD%"=="" echo Alias not found: %NAME%& goto end
set "TMP=%TEMP%\kurono_cmd_input.txt"
>"!TMP!" echo(%CMD%
>>"!TMP!" echo(exit
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-Content \"$env:TEMP\\kurono_cmd_input.txt\" ^| %PY% \"%SIM%\"" > "%LOG_DIR%\last.log"
type "%LOG_DIR%\last.log"
goto end

:alias_remove
set "ALIASES=D:\Important\Kurono\Commands\aliases.txt"
set "NAME=%~2"
if "%NAME%"=="" goto usage
if not exist "%ALIASES%" echo No aliases defined.& goto end
findstr /v /r "^%NAME%=" "%ALIASES%" > "%ALIASES%.tmp"
move /y "%ALIASES%.tmp" "%ALIASES%" >nul
echo Alias removed: %NAME%
goto end

:show_logs
if not exist "%LOG_DIR%\last.log" echo No logs yet.& goto end
type "%LOG_DIR%\last.log"
goto end

:usage
echo Usage: kurono.cmd ^<start^|run^|feed^|test^|alias-add^|alias-list^|alias-run^|alias-remove^|logs^>
echo start                Start interactive Kurono OS
echo run ^<cmd...^>         Execute a single command in Kurono OS
echo feed ^<file^>          Feed commands from a file to Kurono OS
echo test                 Run simulator test suite
echo alias-add ^<name^> ^<cmd...^>   Add alias mapping to a command
echo alias-list           List aliases
echo alias-run ^<name^>    Run alias
echo alias-remove ^<name^> Remove alias
echo logs                 Show last session log

:end
endlocal
