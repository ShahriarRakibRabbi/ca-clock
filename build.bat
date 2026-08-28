@echo off
setlocal enabledelayedexpansion

rem Build CA-CLOCK and its self-tests.
rem
rem g++ is looked up on PATH first, then at the default MinGW location, so the
rem script works whether or not MinGW was added to PATH during installation.

rem Build from the script's own folder so src\ and tests\ resolve no matter
rem where the terminal happens to be.
cd /d "%~dp0"

set "GPP="
for /f "delims=" %%G in ('where g++ 2^>nul') do (
	if not defined GPP set "GPP=%%G"
)
if not defined GPP (
	if exist "C:\MinGW\bin\g++.exe" set "GPP=C:\MinGW\bin\g++.exe"
)
if not defined GPP (
	echo ERROR: g++ not found on PATH or at C:\MinGW\bin\g++.exe
	echo Install MinGW-w64 or add g++ to PATH, then run build.bat again.
	exit /b 1
)

echo Using !GPP!

set "SOURCES=src\config.cpp src\store.cpp src\pagetable.cpp src\arena.cpp src\policy.cpp src\workload.cpp src\stats.cpp src\experiment.cpp"
set "FLAGS=-std=c++17 -O2 -Wall -Wextra -I src"

if not exist results mkdir results

echo Building ca-clock.exe ...
"!GPP!" %FLAGS% -o ca-clock.exe src\main.cpp %SOURCES% -lpsapi
if errorlevel 1 (
	echo BUILD FAILED: ca-clock.exe
	exit /b 1
)

echo Building selftest.exe ...
"!GPP!" %FLAGS% -o selftest.exe tests\selftest.cpp %SOURCES% -lpsapi
if errorlevel 1 (
	echo BUILD FAILED: selftest.exe
	exit /b 1
)

echo Building probe.exe ...
"!GPP!" %FLAGS% -o probe.exe tests\probe_dirty.cpp %SOURCES% -lpsapi
if errorlevel 1 (
	echo BUILD FAILED: probe.exe
	exit /b 1
)

echo.
echo Build OK.  Next  ^(the .\ prefix is required in PowerShell^):
echo.
echo     .\selftest.exe                       all 9 tests
echo     .\probe.exe                          the data-loss probe
echo     .\ca-clock.exe config\demo.ini       the visual demo on its own
echo     .\ca-clock.exe --calibrate           measure this machine's disk latency
echo     .\run_experiments.bat                the full sweep
exit /b 0
