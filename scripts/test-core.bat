@echo off
rem Host tests of the driver cores on a Windows development machine.
rem These are not part of the kernel build: the kernel links the archives made
rem by scripts/build-arm-driver.py against the recorded XNU ABI on macOS.
rem
rem Usage, from the lumia950 directory:  scripts\test-core.bat
rem Any other host, one test at a time:
rem   g++ -std=c++17 -Wall -Wextra -Werror -I drivers/NativeSdhci
rem       drivers/NativeSdhci/Test/check-sdhci-core.cpp drivers/NativeSdhci/SdhciCore.cpp
rem       -o build/check-sdhci-core
rem   g++ -std=c++17 -Wall -Wextra -Werror -I drivers/NativeAdreno418
rem       drivers/NativeAdreno418/Test/check-adreno-core.cpp drivers/NativeAdreno418/AdrenoCore.cpp
rem       drivers/NativeAdreno418/IommuCore.cpp drivers/NativeAdreno418/CommandStream.cpp
rem       -o build/check-adreno-core
rem   g++ -std=c++17 -Wall -Wextra -Werror -I drivers/NativeMdss
rem       drivers/NativeMdss/Test/check-mdss-core.cpp drivers/NativeMdss/MdssCore.cpp
rem       -o build/check-mdss-core

setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo No Visual Studio 2022 x64 environment at "%VCVARS%".
  echo Use the host compiler lines above instead.
  exit /b 2
)

call "%VCVARS%" >nul 2>&1 || exit /b 2
if not exist build mkdir build

cl /nologo /std:c++17 /EHsc /W4 /WX /I drivers\NativeSdhci ^
   drivers\NativeSdhci\Test\check-sdhci-core.cpp drivers\NativeSdhci\SdhciCore.cpp ^
   /Fe:build\check-sdhci-core.exe /Fo:build\ || exit /b 1
build\check-sdhci-core.exe || exit /b 1

cl /nologo /std:c++17 /EHsc /W4 /WX /I drivers\NativeAdreno418 ^
   drivers\NativeAdreno418\Test\check-adreno-core.cpp drivers\NativeAdreno418\AdrenoCore.cpp ^
   drivers\NativeAdreno418\IommuCore.cpp drivers\NativeAdreno418\CommandStream.cpp ^
   /Fe:build\check-adreno-core.exe /Fo:build\ || exit /b 1
build\check-adreno-core.exe || exit /b 1

cl /nologo /std:c++17 /EHsc /W4 /WX /I drivers\NativeMdss ^
   drivers\NativeMdss\Test\check-mdss-core.cpp drivers\NativeMdss\MdssCore.cpp ^
   /Fe:build\check-mdss-core.exe /Fo:build\ || exit /b 1
build\check-mdss-core.exe || exit /b 1
