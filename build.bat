@echo off
rem Builds build\AutoClicker.exe
rem Needs ONE of:
rem   - Zig          (winget install zig.zig)   or  pip install ziglang
rem   - MinGW-w64    (gcc + windres on PATH, e.g. from MSYS2 or w64devkit)
setlocal
if not exist build mkdir build
set LIBS=-luser32 -lgdi32 -lcomctl32 -lwinmm -lmsimg32

where zig >nul 2>nul
if %errorlevel%==0 (
  zig cc -target x86_64-windows-gnu -O2 -s -municode -Wl,--subsystem,windows src\autoclicker.c res\app.rc -o build\AutoClicker.exe %LIBS%
  goto done
)
python -c "import ziglang" >nul 2>nul
if %errorlevel%==0 (
  python -m ziglang cc -target x86_64-windows-gnu -O2 -s -municode -Wl,--subsystem,windows src\autoclicker.c res\app.rc -o build\AutoClicker.exe %LIBS%
  goto done
)
where gcc >nul 2>nul
if %errorlevel%==0 (
  windres -I res res\app.rc -O coff -o build\app.res.o || goto fail
  gcc -O2 -s -municode -mwindows src\autoclicker.c build\app.res.o -o build\AutoClicker.exe %LIBS% || goto fail
  goto done
)
echo No compiler found. Install Zig (winget install zig.zig) or MinGW-w64, then run this again.
exit /b 1
:fail
echo Build failed.
exit /b 1
:done
if exist build\AutoClicker.exe echo Built build\AutoClicker.exe
