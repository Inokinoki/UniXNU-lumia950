@echo off
rem Host test of the NativeSdhci core logic on a Windows development machine.
rem This is not part of the kernel build: the kernel links the archive produced
rem by scripts/build-arm-driver.py against the recorded XNU ABI on macOS.
rem
rem Usage, from the lumia950 directory:  scripts\test-core.bat
rem Any other host:  g++ -std=c++17 -Wall -Wextra -Werror -I drivers/NativeSdhci
rem   drivers/NativeSdhci/Test/check-sdhci-core.cpp drivers/NativeSdhci/SdhciCore.cpp
rem   -o build/check-sdhci-core

setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo No Visual Studio 2022 x64 environment at "%VCVARS%".
  echo Use the host compiler line above instead.
  exit /b 2
)

call "%VCVARS%" >nul 2>&1 || exit /b 2
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W4 /WX /I drivers\NativeSdhci ^
   drivers\NativeSdhci\Test\check-sdhci-core.cpp drivers\NativeSdhci\SdhciCore.cpp ^
   /Fe:build\check-sdhci-core.exe /Fo:build\ || exit /b 1
build\check-sdhci-core.exe
