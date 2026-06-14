@echo off
REM Wrapper: initialise MSVC env then invoke cl with all passed args.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl %*
