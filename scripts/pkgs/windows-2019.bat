@echo off
@setlocal enableextensions enabledelayedexpansion

net session >nul: 2>&1
if errorlevel 1 (
	echo %0 must be run with Administrator privileges
	goto :eof
)

:: Use PowerShell to install Chocolatey
set "PATH=%ALLUSERSPROFILE%\chocolatey\bin;%PATH%"
echo [1/5] Install: Chocolatey Manager
powershell -NoProfile -InputFormat None -ExecutionPolicy Bypass -Command "[System.Net.ServicePointManager]::SecurityProtocol = 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://chocolatey.org/install.ps1'))"
where /q choco
if errorlevel 1 (
	echo [1/5] Install: Chocolatey = FAIL
	goto :eof
)

:: Use Chocolatey to install msys2
echo [2/5] Install: MSYS2
set "PATH=%SystemDrive%\tools\msys64;%PATH%"
choco install msys2 -y -r
where /q msys2
if errorlevel 1 (
	echo [2/5] Install: MSYS2 = FAIL
	goto :eof
)
echo [2/5] Install: MSYS2 = PASS

:: Use MSYS2/pacman to install gcc and clang toolchain
set MSYS2=call msys2_shell -no-start -here -use-full-path -defterm

echo [3/5] Install: MinGW Toolchain via msys2/pacman
set "PATH=%SystemDrive%\tools\msys64\mingw64\bin;%PATH%"
%MSYS2% -c "pacman --noconfirm -Syy --needed base-devel mingw-w64-x86_64-toolchain"
where /q gcc
if errorlevel 1 (
	echo [4/5] Install: MinGW Toolchain via msys2/pacman = FAIL
	goto :eof
)
echo [3/5] Install: MinGW Toolchain via msys2/pacman = PASS

echo 4/5] Install: MinGW/clang via msys2/pacman
%MSYS2% -c "pacman --noconfirm -Syy mingw-w64-x86_64-clang"
where /q clang
if errorlevel 1 (
	echo [5/5] Install: MinGW Toolchain via msys2/pacman = FAIL
	goto :eof
)
echo [4/5] Install: MinGW Toolchain via msys2/pacman = PASS

echo [5/5] Install: MinGW/nsis via msys2/pacman
%MSYS2% -c "pacman --noconfirm -Syy mingw-w64-x86_64-nsis"
where /q nsis
if errorlevel 1 (
	echo [6/6] Install: MinGW/nsis via msys2/pacman = FAIL
	goto :eof
)
echo [5/5] Install: MinGW/nsis via msys2/pacman = OK
