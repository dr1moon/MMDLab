@echo off
setlocal EnableExtensions

set "ProjectRoot=%~dp0"
set "PremakePath=%ProjectRoot%Tools\premake5.exe"
set "VsWherePath=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "SolutionPath=%ProjectRoot%MMDLab.slnx"
set "BuildSolution=0"
set "Configuration=Debug"

:ParseArguments
if "%~1"=="" goto ValidateTools

if /I "%~1"=="--build" (
    set "BuildSolution=1"
    shift
    goto ParseArguments
)

if /I "%~1"=="Debug" (
    set "Configuration=Debug"
    shift
    goto ParseArguments
)

if /I "%~1"=="Release" (
    set "Configuration=Release"
    shift
    goto ParseArguments
)

echo Unknown argument: %~1
echo Usage: %~nx0 [--build] [Debug^|Release]
exit /b 1

:ValidateTools
if not exist "%PremakePath%" (
    echo Premake was not found: "%PremakePath%"
    exit /b 1
)

if not exist "%VsWherePath%" (
    echo vswhere was not found: "%VsWherePath%"
    exit /b 1
)

set "MsBuildPath="
for /f "usebackq delims=" %%I in (`call "%VsWherePath%" -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe"`) do set "MsBuildPath=%%I"

if not defined MsBuildPath (
    echo MSBuild was not found in an installed Visual Studio instance.
    exit /b 1
)

echo Using MSBuild: "%MsBuildPath%"
echo Generating Visual Studio 2026 projects...
call "%PremakePath%" --file="%ProjectRoot%premake5.lua" vs2026
if errorlevel 1 exit /b %errorlevel%

if "%BuildSolution%"=="0" (
    echo Generated "%SolutionPath%"
    echo Run "%~nx0 --build Debug" to build the solution.
    exit /b 0
)

echo Building %Configuration% x64...
call "%MsBuildPath%" "%SolutionPath%" /m /p:Configuration=%Configuration% /p:Platform=x64 /verbosity:minimal
exit /b %errorlevel%
