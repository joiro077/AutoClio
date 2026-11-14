# Compila o simulador do painel para Windows e gera um PNG de cada tela.
# Precisa do Zig (scoop install zig) e do LVGL baixado pelo ESP-IDF
# (pasta managed_components, criada pelo primeiro "idf.py build").
#
#   powershell -File tools\sim\build.ps1 [-Lvgl <pasta do lvgl>] [-Out <pasta de saida>]
param(
    [string]$Lvgl = "",
    [string]$Out = ""
)
$ErrorActionPreference = 'Stop'
$sim = $PSScriptRoot
$proj = (Resolve-Path "$sim\..\..").Path
if (-not $Lvgl) { $Lvgl = "$proj\managed_components\lvgl__lvgl" }
if (-not (Test-Path "$Lvgl\lvgl.h")) { throw "LVGL nao encontrado em $Lvgl (rode idf.py build uma vez, ou passe -Lvgl)" }
if (-not $Out) { $Out = "$sim\out" }
New-Item -ItemType Directory -Force $Out | Out-Null
$build = "$sim\build"
New-Item -ItemType Directory -Force $build | Out-Null

$src = @("$sim\sim_main.c")
$src += 'obd_task.c', 'app_state.c', 'logic_task.c', 'font_big88.c' | ForEach-Object { "$proj\main\app\$_" }
$src += (Get-ChildItem "$proj\main\core" -Filter *.c).FullName
$src += (Get-ChildItem "$Lvgl\src" -Recurse -Filter *.c).FullName

$flags = @('-target', 'x86_64-windows-gnu', '-O1', '-w', '-fno-sanitize=undefined', '-DLV_CONF_INCLUDE_SIMPLE',
    "-I$sim", "-I$sim\shim", "-I$proj\main", "-I$proj\main\core", "-I$proj\main\app", "-I$Lvgl")
$rsp = "$build\args.rsp"
($flags + $src + @('-lwinmm', '-o', "$build\painel_sim.exe")) | ForEach-Object { '"' + ($_ -replace '\\', '/') + '"' } |
    Set-Content -Encoding ascii $rsp
& zig cc "@$rsp"
if ($LASTEXITCODE -ne 0) { throw "falha na compilacao" }

& "$build\painel_sim.exe" $Out
if ($LASTEXITCODE -ne 0) { throw "o simulador terminou com erro $LASTEXITCODE" }

# BMP -> PNG
Add-Type -AssemblyName System.Drawing
Get-ChildItem $Out -Filter *.bmp | ForEach-Object {
    $img = [System.Drawing.Image]::FromFile($_.FullName)
    $img.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    Remove-Item $_.FullName
}
Write-Output "PNG em $Out"