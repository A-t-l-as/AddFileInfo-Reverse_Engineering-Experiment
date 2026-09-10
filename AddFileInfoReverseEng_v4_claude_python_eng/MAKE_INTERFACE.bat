@echo off

rem --- Setting variables ---
SET INTERFACE_NAME="My great interface"
SET INTERFACE_ID=JS

echo Dialogs%INTERFACE_ID%>Interface%INTERFACE_ID%.def
echo Cursors%INTERFACE_ID%>>Interface%INTERFACE_ID%.def
echo Compass%INTERFACE_ID%>>Interface%INTERFACE_ID%.def

python AddFileInfo2026_RE.py Interface%INTERFACE_ID%.def Interface%INTERFACE_ID%.int -text -name %INTERFACE_NAME%

pause
