@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul
set PYTHONIOENCODING=utf-8
set PYTHONUTF8=1
set "KURONO_DIR=%~dp0"
set "SIM=%KURONO_DIR%kurono_os_sim.py"
set "EXE=%KURONO_DIR%bin\kurono_os_cpp.exe"
set "LOG_DIR=%KURONO_DIR%logs"
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
set "KURONO_BASE=%KURONO_DIR%"
if exist "%EXE%" (
  "%EXE%"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "& { $log = '\"%LOG_DIR%\\last.log\"'; python '\"%SIM%\"' | Tee-Object -FilePath $log }"
)
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
%PY% "%SIM%" --cmd "!LINE:~1!" > "%LOG_DIR%\last.log"
type "%LOG_DIR%\last.log"
goto end

:feed
set "FILE=%~2"
if "%FILE%"=="" goto usage
if not exist "%FILE%" echo File not found: %FILE% & goto end
%PY% "%SIM%" --file "%FILE%" > "%LOG_DIR%\last.log"
type "%LOG_DIR%\last.log"
goto end

:test
if exist "%KURONO_DIR%bin\test_suite_cpp.exe" (
  "%KURONO_DIR%bin\test_suite_cpp.exe" --test > "%LOG_DIR%\last.log"
  type "%LOG_DIR%\last.log"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new(); %PY% \"%SIM%\" --test" > "%LOG_DIR%\last.log"
  type "%LOG_DIR%\last.log"
)
goto end

:alias_add
set "ALIASES=%KURONO_DIR%Commands\aliases.txt"
if not exist "%KURONO_DIR%Commands" mkdir "%KURONO_DIR%Commands"
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
set "ALIASES=%KURONO_DIR%Commands\aliases.txt"
if not exist "%ALIASES%" echo No aliases defined.& goto end
type "%ALIASES%"
goto end

:alias_run
set "ALIASES=%KURONO_DIR%Commands\aliases.txt"
set "NAME=%~2"
if "%NAME%"=="" goto usage
if not exist "%ALIASES%" echo No aliases defined.& goto end
set "CMD="
for /f "usebackq tokens=1* delims==" %%A in ("%ALIASES%") do if /I "%%A"=="%NAME%" set "CMD=%%B"
if "%CMD%"=="" echo Alias not found: %NAME%& goto end
%PY% "%SIM%" --cmd "%CMD%" > "%LOG_DIR%\last.log"
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
echo Usage: kurono_complex.cmd ^<start^|run^|feed^|test^|alias-add^|alias-list^|alias-run^|alias-remove^|logs^>
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