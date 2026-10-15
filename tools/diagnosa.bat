@echo off
REM Launcher cepat untuk Windows. Klik 2x atau jalankan dari CMD.
REM Pakai argumen opsional, contoh:  diagnosa.bat --mirror
setlocal
cd /d "%~dp0"
python diagnosa.py %*
echo.
pause
