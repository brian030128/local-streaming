@echo off
rem Builds sender.exe and receiver.exe with whichever supported compiler is on PATH.
rem Both are statically linked, so you can copy them to another PC without installing anything.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if %errorlevel%==0 goto msvc
where clang++ >nul 2>nul
if %errorlevel%==0 ( set "CXX=clang++" & goto gnu )
where g++ >nul 2>nul
if %errorlevel%==0 ( set "CXX=g++" & goto gnu )

echo No C++ compiler found.
echo Run this from a "Developer Command Prompt for VS", or put llvm-mingw on PATH.
exit /b 1

:msvc
set "LIBS=ws2_32.lib d3d11.lib dxgi.lib d3dcompiler.lib mf.lib mfplat.lib mfuuid.lib ole32.lib oleaut32.lib user32.lib gdi32.lib winmm.lib"
cl /nologo /O2 /EHsc /std:c++17 /MT /W3 /Ithird_party\vpl sender.cpp /Fe:sender.exe /link %LIBS% || exit /b 1
cl /nologo /O2 /EHsc /std:c++17 /MT /W3 receiver.cpp /Fe:receiver.exe /link %LIBS% || exit /b 1
cl /nologo /O2 /EHsc /std:c++17 /MT /W3 /D_WIN32_WINNT=0x0A00 latency_clock.cpp /Fe:latency_clock.exe /link %LIBS% || exit /b 1
goto done

:gnu
set "LIBS=-lws2_32 -ld3d11 -ldxgi -ld3dcompiler -lmf -lmfplat -lmfuuid -lole32 -loleaut32 -luser32 -lgdi32 -lwinmm"
%CXX% -O2 -std=c++17 -static -Ithird_party/vpl sender.cpp -o sender.exe %LIBS% || exit /b 1
%CXX% -O2 -std=c++17 -static receiver.cpp -o receiver.exe %LIBS% || exit /b 1
%CXX% -O2 -std=c++17 -static -D_WIN32_WINNT=0x0A00 latency_clock.cpp -o latency_clock.exe %LIBS% || exit /b 1

:done
echo Built sender.exe, receiver.exe and latency_clock.exe
