param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("standard", "round", "rect")]
    [string]$Board,

    [Parameter(Mandatory = $true)]
    [ValidateSet(
        "factory",
        "xl9555",
        "lora_sx1276",
        "lora_sx1262",
        "lora_lr1121",
        "lora_lr2021",
        "axp517",
        "lcd",
        "lcd1",
        "lcd_touch",
        "audio_play_wav",
        "audio_record",
        "audio_loopback",
        "camera",
        "camera_esp_video",
        "camera_flash_led",
        "esp32p4_host_wifi",
        "esp32p4_host_test",
        "sd",
        "sgm38121",
        "drv2605",
        "qmc6309",
        "qmi8658c",
        "lvgl",
        "usb_otg_msc",
        "codex_keyboard"
    )]
    [string]$Example,

    [switch]$Build
)

$ErrorActionPreference = "Stop"

$workspace = Split-Path -Parent $PSScriptRoot
$settingsPath = Join-Path $workspace ".vscode\settings.json"
$variant = "$Board-$Example"
$buildRelative = "build\$variant"
$buildPathSetting = '${workspaceFolder}\' + $buildRelative
$sdkconfigPathSetting = "$buildPathSetting\sdkconfig"

$standardRoundOnly = @(
    "xl9555",
    "lora_sx1276",
    "lora_sx1262",
    "lora_lr1121",
    "lora_lr2021",
    "esp32p4_host_wifi",
    "esp32p4_host_test"
)
if ($Board -eq "rect" -and $standardRoundOnly -contains $Example) {
    throw "Example '$Example' is not supported by the rect board."
}

$rectOnly = @("camera_flash_led", "sensor", "drv2605", "qmc6309", "qmi8658c")
if ($Board -ne "rect" -and $rectOnly -contains $Example) {
    throw "Example '$Example' is only supported by the rect board."
}

if ($Example -eq "lcd1" -and $Board -ne "round") {
    throw "Example 'lcd1' is only supported by the round board."
}

if (-not (Test-Path -LiteralPath $settingsPath)) {
    throw "VS Code settings file not found: $settingsPath"
}

$settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
$settings.'idf.buildPathWin' = $buildPathSetting
$settings.'idf.sdkconfigFilePath' = $sdkconfigPathSetting
$settings.'idf.cmakeCompilerArgs' = @(
    "-G",
    "Ninja",
    "-DPYTHON_DEPS_CHECKED=1",
    "-DESP_PLATFORM=1",
    "-DT_PANEL_P4_BOARD=$Board",
    "-DT_PANEL_P4_EXAMPLE=$Example"
)

$settingsJson = $settings | ConvertTo-Json -Depth 20
[System.IO.File]::WriteAllText($settingsPath, $settingsJson + [Environment]::NewLine)

Write-Host "Selected T-Panel-P4 variant: $Board + $Example"
Write-Host "Build directory: $buildRelative"

if (-not $Build) {
    Write-Host "ESP-IDF Build, Flash, and Monitor commands now use this variant."
    exit 0
}

$idfPath = $settings.'idf.espIdfPathWin'
$idfToolsPath = $settings.'idf.toolsPathWin'
if (-not $idfPath -or -not (Test-Path -LiteralPath "$idfPath\export.ps1")) {
    throw "Invalid idf.espIdfPathWin in .vscode/settings.json: $idfPath"
}
if ($idfToolsPath) {
    $env:IDF_TOOLS_PATH = $idfToolsPath
}
$env:IDF_TARGET = "esp32p4"

$previousLocation = Get-Location
try {
    Set-Location -LiteralPath $idfPath
    . .\export.ps1
    Set-Location -LiteralPath $workspace

    & idf.py `
        -B $buildRelative `
        -D "T_PANEL_P4_BOARD=$Board" `
        -D "T_PANEL_P4_EXAMPLE=$Example" `
        build
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}
finally {
    Set-Location $previousLocation
}
