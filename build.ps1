# vi: ts=4 sw=4 ff=unix fenc=utf-8
# redntfy ビルドスクリプト
# DevShell モジュール経由で VC++ ビルド環境を初期化し、rc/cl でビルドする。
param([string]$Version = "0.0.0", [switch]$Release, [switch]$Test)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$Version = $Version -replace '^v', ''

# vcpkg パス設定（VCPKG_INSTALLATION_ROOT 環境変数 → Scoop シム の優先順）
if ($env:VCPKG_INSTALLATION_ROOT) {
    $vcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
}
else {
    $vcpkgCmd = (Get-Command vcpkg -ErrorAction Stop).Source
    $vcpkgRoot = Split-Path $vcpkgCmd
    $shimFile = [System.IO.Path]::ChangeExtension($vcpkgCmd, ".shim")
    if (Test-Path $shimFile) {
        $vcpkgReal = (Get-Content $shimFile |
            Where-Object { $_ -match "^path" } |
            ForEach-Object { ($_ -split '"')[1] } |
            Select-Object -First 1)
        if ($vcpkgReal) {
            $vcpkgRoot = Split-Path $vcpkgReal
        }
    }
}
$vcpkgInclude = "$vcpkgRoot\installed\x64-windows-static\include"
$vcpkgLib     = "$vcpkgRoot\installed\x64-windows-static\lib"

# 依存ライブラリのインストール（未インストール時のみ実行）
& "$vcpkgRoot\vcpkg.exe" install libebur128:x64-windows-static
if ($LASTEXITCODE) { exit 1 }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Write-Error "vswhere.exe が見つからない: $vswhere"; exit 1 }
$vsPath = & $vswhere -products '*' -latest -property installationPath
if (-not $vsPath) { Write-Error "Visual Studio / Build Tools が見つからない"; exit 1 }

$devShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

"#define APP_VERSION L`"$Version`"" | Set-Content -Encoding UTF8NoBOM out\version.h

# 単体テストのビルド（-Test 時のみ。rc は不要で、最適化なしでコンパイル時間を優先する）
# テストはコンソール exe のため main をエントリに明示し、main.cpp 側の wmain と共存させる
if ($Test) {
    cl /nologo /utf-8 /std:c++20 /EHsc /Od /I out\ /I "$vcpkgInclude" `
        /Foout\ /Feout\redntfy_test.exe `
        tests\test_main.cpp `
        /link /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup `
        windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib gdi32.lib `
        "$vcpkgLib\ebur128.lib"
    if ($LASTEXITCODE) { exit 1 }
    exit 0
}

rc /nologo /fo out\resource.res src\resource.rc
if ($LASTEXITCODE) { exit 1 }

# リリースモード時の追加フラグ
$clExtra   = if ($Release) { @('/DNDEBUG', '/GL', '/Gy') } else { @() }
$linkExtra = if ($Release) { @('/LTCG', '/OPT:REF', '/OPT:ICF') } else { @() }

cl /nologo /utf-8 /std:c++20 /EHsc /O2 @clExtra /I out\ /I "$vcpkgInclude" `
    /Foout\ /Feout\redntfy.exe `
    src\main.cpp out\resource.res `
    /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup @linkExtra `
    windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib gdi32.lib `
    "$vcpkgLib\ebur128.lib"
if ($LASTEXITCODE) { exit 1 }
