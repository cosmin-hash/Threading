# Launch the Threading Model Visualiser (puts the Qt + MinGW runtime on PATH).
#
# Edit the two defaults below to match your Qt installation, or override them
# without editing this file by setting $env:QT_DIR / $env:MINGW_DIR first, e.g.
#   $env:QT_DIR = "C:\Qt\6.9.2\mingw_64"; .\run.ps1
$qt    = if ($env:QT_DIR)    { $env:QT_DIR }    else { "C:\Qt\6.9.2\mingw_64" }
$mingw = if ($env:MINGW_DIR) { $env:MINGW_DIR } else { "C:\Qt\Tools\mingw1310_64\bin" }

$env:PATH = "$qt\bin;$mingw;$env:PATH"
& "$PSScriptRoot\build\ThreadingViz.exe"
