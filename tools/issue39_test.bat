@echo off
rem Issue 39, the thin lines in the cloud sea on AMD: runs the game once per setting and zips what each run saw.
rem Put this file next to xc2.exe and double-click it. It needs no input and changes no settings or saves. Each run
rem sets its switches for that run only. It takes about 6 minutes; the game window opens and closes by itself.
setlocal
cd /d "%~dp0"
if not exist xc2.exe (
    echo Put this file in the folder with xc2.exe, then run it again.
    pause
    exit /b 1
)
set "OUT=issue39-results"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"

rem The same scene and pace in every run: the title screen, no input, no cutscene frame cap. Shots a third of a second
rem apart at the 3D frame's size show the one-pixel lines even if they come and go; the thumbnails, too small for that,
rem measure the flicker. The thumbnail ring must stay well above what is written while frames go on, or it wraps during
rem the dump and loses them.
set XC2_INPUT=none
set XC2_ASSUME_FOCUS=1
set XC2_BACKGROUND_WINDOW=1
set XC2_CUTSCENE_30=0
set XC2_FPS_LIMIT=60
set XC2_FLIGHT=1
set XC2_FLIGHT_FRAMES=160
set XC2_FLIGHT_SPIKE_MS=0
set XC2_FLIGHT_AT=600
set XC2_FLIGHT_EXIT=1
set XC2_SHOTS=550-590:20
set XC2_SHOTS_SIZE=1280x720

rem Each run ends when its dump is written; the seconds are only the limit for a run that never gets there.
call :run 01-base 60
call :run 02-compress-off 60 XC2_TEX_COMPRESS=0
call :run 03-compress-storage 60 XC2_TEX_COMPRESS=storage
call :run 04-oversize-own 60 XC2_TEX_OVERSIZE=0
call :run 05-targets-own 60 XC2_TEX_OVERSIZE=all
call :run 06-feedback-read 60 XC2_FEEDBACK_READ=1
call :run 07-scale-100 60 XC2_RENDER_SCALE=100
rem These two change the shaders, which translate again, so they get longer.
call :run 08-gather-off 90 XC2_GATHER_BIAS=0
call :run 09-subgroup-free 90 XC2_VK_SUBGROUP_PIN=0
call :run 10-a2c-off 60 XC2_VK_DYN_A2C=0
call :run 11-gpl-off 90 XC2_GPL=0
call :run 12-replay-sync 60 XC2_REPLAY_ASYNC=0
call :run 13-feedback-restart 60 XC2_FEEDBACK=restart
rem Every pass of one frame as pictures, which shows the first pass the lines appear in whatever their cause. The dump
rem takes a while to finish, so this run ends later.
call :run 14-frame-dump 90 XC2_DUMP=590 XC2_DUMP_UBO=1 XC2_DUMP_DIR=%CD%\%OUT%\14-frame-dump\dump XC2_FLIGHT_AT=900

echo.
echo Packing the results...
tar -a -c -f issue39-results.zip "%OUT%"
echo.
echo Done. Please attach issue39-results.zip to the issue, or share it by a link.
pause
exit /b 0

rem One run: %1 the folder name, %2 its seconds, the rest NAME=VALUE switches, which cmd passes as NAME and VALUE.
:run
echo.
echo Run %1
setlocal
set "NAME=%~1"
set "SECS=%~2"
mkdir "%OUT%\%NAME%"
shift
shift
:switch
if "%~1"=="" goto go
set "%~1=%~2"
shift
shift
goto switch
:go
set "XC2_FLIGHT_DIR=%CD%\%OUT%\%NAME%\flight"
set "XC2_SHOTS_DIR=%CD%\%OUT%\%NAME%"
"%CD%\xc2.exe" --seconds %SECS% 2> "%OUT%\%NAME%\log.txt"
echo exit code %errorlevel% >> "%OUT%\%NAME%\log.txt"
if not exist "%OUT%\%NAME%\f*.png" echo This run took no picture; its log may say why.
endlocal
exit /b 0
