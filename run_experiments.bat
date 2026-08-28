@echo off
setlocal enabledelayedexpansion

rem ---------------------------------------------------------------------------
rem CA-CLOCK parameter sweep.
rem
rem One process per run, so a crash costs one run instead of the whole sweep.
rem Every run appends a row to results\runs.csv.
rem
rem Before the sweep it calibrates the device ONCE. All runs are then compared
rem using that single pair of latencies, because per-run latency drifts by up to
rem 2x on an idle laptop and would otherwise swamp the effect being measured.
rem
rem Defaults below take roughly 20-30 minutes. Raise ACCESSES for a stronger
rem result, lower it if you just want the shape of the curves.
rem ---------------------------------------------------------------------------

set TOTAL_PAGES=4096
set FRAME_BUDGET=256
set ACCESSES=60000
set PATTERN=zipf
set ZIPF_S=0.99
set DURABLE=false

rem Policy arms. "a<x>" = probabilistic sparing with alpha = x.
rem                "k<n>" = bounded second chance with at most n skips per page.
rem a0.00 is plain CLOCK and is the baseline every other arm is compared against.
set ARMS=a0.00 a0.25 a0.50 a0.75 a1.00 k1 k2 k4 k8
set WRITE_RATIOS=0.20 0.40 0.80
set SEEDS=1 2 3

rem Run from the script's own folder so config\ and results\ paths resolve
rem no matter where the terminal happens to be.
cd /d "%~dp0"

if not exist "%~dp0ca-clock.exe" (
	echo ERROR: ca-clock.exe not found. Run build.bat first.
	exit /b 1
)
if not exist results mkdir results

echo Removing any previous sweep output ...
if exist results\runs.csv del results\runs.csv

echo.
echo === Calibrating device (once) ===
"%~dp0ca-clock.exe" config\experiment.ini --calibrate
if errorlevel 1 (
	echo Calibration failed.
	exit /b 1
)

set /a RUN=0
set /a TOTAL=0
for %%P in (%ARMS%) do for %%W in (%WRITE_RATIOS%) do for %%S in (%SEEDS%) do set /a TOTAL+=1

echo.
echo === Sweep: !TOTAL! runs ===
for %%P in (%ARMS%) do (
	set "ARM=%%P"
	set "KIND=!ARM:~0,1!"
	set "VALUE=!ARM:~1!"
	if /i "!KIND!"=="a" (
		set "ALPHA=!VALUE!"
		set "SKIPS=0"
	) else (
		set "ALPHA=0.0"
		set "SKIPS=!VALUE!"
	)

	for %%W in (%WRITE_RATIOS%) do (
		for %%S in (%SEEDS%) do (
			set /a RUN+=1
			set "TMPCFG=results\_sweep.ini"
			>  "!TMPCFG!" echo total_pages     = %TOTAL_PAGES%
			>> "!TMPCFG!" echo frame_budget    = %FRAME_BUDGET%
			>> "!TMPCFG!" echo access_count    = %ACCESSES%
			>> "!TMPCFG!" echo alpha           = !ALPHA!
			>> "!TMPCFG!" echo max_dirty_skips = !SKIPS!
			>> "!TMPCFG!" echo write_ratio     = %%W
			>> "!TMPCFG!" echo pattern         = %PATTERN%
			>> "!TMPCFG!" echo zipf_s          = %ZIPF_S%
			>> "!TMPCFG!" echo durable_writes  = %DURABLE%
			>> "!TMPCFG!" echo seed            = %%S
			>> "!TMPCFG!" echo backing_path    = results/backing.dat
			>> "!TMPCFG!" echo csv_path        = results/runs.csv
			>> "!TMPCFG!" echo label           = !ARM!-w%%W-s%%S

			echo [!RUN!/!TOTAL!] !ARM! write_ratio=%%W seed=%%S
			"%~dp0ca-clock.exe" "!TMPCFG!" | findstr /C:"[io]"
			if errorlevel 1 echo    ^(run failed, continuing^)
		)
	)
)

if exist results\_sweep.ini del results\_sweep.ini

echo.
echo Sweep complete. Results in results\runs.csv
echo Next:  python analysis\analyse.py results\runs.csv
exit /b 0
