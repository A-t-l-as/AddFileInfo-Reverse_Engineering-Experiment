# --- Ustawianie zmiennych ---
$InterfaceName = "My great interface"
$InterfaceId   = "JS"

# --- Tworzenie pliku .def ---
$defFile = "Interface$InterfaceId.def"

"Dialogs$InterfaceId"  | Out-File -FilePath $defFile -Encoding ASCII
"Cursors$InterfaceId"  | Out-File -FilePath $defFile -Encoding ASCII -Append
"Compass$InterfaceId"  | Out-File -FilePath $defFile -Encoding ASCII -Append

# --- Wywolanie skryptu Pythona ---
$intFile = "Interface$InterfaceId.int"
python AddFileInfo2026_RE.py $defFile $intFile -text -name $InterfaceName

# --- Pauza (odpowiednik "pause") ---
Read-Host "Press enter to continue ..."