# Launch the Threading Model Visualiser.
# Puts the Qt + MinGW runtime on PATH, then starts the built exe.
# Override the defaults with the QT_DIR / MINGW_DIR environment variables.
$qt    = if ($env:QT_DIR)    { $env:QT_DIR }    else { "C:\Qt\6.9.2\mingw_64" }
$mingw = if ($env:MINGW_DIR) { $env:MINGW_DIR } else { "C:\Qt\Tools\mingw1310_64\bin" }
$env:PATH = "$qt\bin;$mingw;$env:PATH"
& "$PSScriptRoot\build\ThreadingViz.exe"
