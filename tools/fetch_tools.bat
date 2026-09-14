@echo off
setlocal
cd /d "%~dp0"
for %%T in (jenny midimix) do (
    echo fetching the latest %%T.exe
    curl -fsSL -o %%T.exe https://github.com/twobob/%%T/releases/latest/download/%%T.exe || (echo could not download %%T.exe & exit /b 1)
)
echo done: tools\jenny.exe and tools\midimix.exe
