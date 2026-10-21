@echo off
setlocal
set "ENGINE_EXE=%~dp0..\build-rel\bin\SaidaEngine.exe"
if not exist "%ENGINE_EXE%" (
    echo SaidaEngine.exe introuvable. Depuis la racine du moteur, compiler avec :
    echo cmake --build build-rel --target SaidaEngine --parallel 2
    exit /b 1
)
if exist "C:\msys64\ucrt64\bin" set "PATH=C:\msys64\ucrt64\bin;C:\msys64\usr\bin;%PATH%"
"%ENGINE_EXE%" --project "%~dp0VRDemo.saidaproj"
exit /b %ERRORLEVEL%
