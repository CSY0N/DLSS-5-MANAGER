@echo off
rem LIVE FLOW for AMD Radeon (ALPHA): the same worker built with LF_RADEON,
rem the network evaluate replaced by radeon\radeon_pass.inl. Output:
rem radeon\out\liveflow_radeon.exe - deployed to mod files\native\radeon\.
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist radeon\out mkdir radeon\out
cl /nologo /O2 /EHsc /W3 /MD /utf-8 /std:c++17 /DLF_RADEON /Iinclude /Foradeon\out\ lf_worker.cpp lf_share.cpp radeon\radeon_engine.cpp radeon\radeon_fsr.cpp /Fe:radeon\out\liveflow_radeon.exe /link lib\Windows_x86_64\x64\nvsdk_ngx_d.lib SpoutDX.lib version.lib kernel32.lib user32.lib gdi32.lib advapi32.lib ole32.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib WindowsApp.lib dwmapi.lib bcrypt.lib
exit /b %errorlevel%
