@echo off
rem Supply --nsp "your.nsp" --keys "your prod.keys", or set HWDER_NSP and HWDER_KEYS.
python "%~dp0tools\first_run.py" --launch %*
if errorlevel 1 pause
