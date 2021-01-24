@echo off
mkdir BuildDirectory
pushd BuildDirectory

SET b=Debug

IF %b%==Debug (
set CommonCStuff=/D WIN32=1 /Zi /Od /Fd /MTd 
) ELSE (
REM Release
set CommonCStuff=-fp:fast -fp:except- /D WIN32=1 /O2 /Fd /MT 
)

set CommonLinkerFlags=-STACK:0x100000,0x100000 -incremental:no -opt:ref user32.lib opengl32.lib gdi32.lib winmm.lib kernel32.lib Shell32.lib
cl.exe /LD %CommonCStuff% /D_USRDLL /D_WINDLL ../xrns_player.c /Fe"XRNSPlayer.dll" /I ../ /link %CommonLinkerFlags%
move XRNSPlayer.dll "../XRNSPlayer.dll"
popd
