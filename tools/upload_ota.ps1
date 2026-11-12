param(
    [Parameter(Mandatory = $true)]
    [string]$Address,

    [Parameter(Mandatory = $true)]
    [string]$Firmware,

    [string]$Token,

    [ValidateRange(180, 1800)]
    [int]$UploadTimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Firmware -PathType Leaf)) {
    throw "Firmware file not found: $Firmware"
}

if ([string]::IsNullOrWhiteSpace($Token)) {
    $tokenPath = Join-Path (Split-Path -Parent $PSScriptRoot) '.ota-token'
    if (-not (Test-Path -LiteralPath $tokenPath -PathType Leaf)) {
        throw 'OTA token is missing. Supply -Token or restore the project .ota-token file.'
    }
    $Token = (Get-Content -Raw -LiteralPath $tokenPath).Trim()
}

if ($Token -notmatch '^[0-9a-fA-F]{64}$') {
    throw 'OTA token must contain exactly 64 hexadecimal characters.'
}

$baseUri = "http://$Address"
$status = Invoke-RestMethod -Uri "$baseUri/api/ota" -TimeoutSec 10
if (-not $status.enabled) {
    throw "OTA is not enabled on $Address (next partition: '$($status.next_partition)')."
}

$firmwareFile = Get-Item -LiteralPath $Firmware
if ($firmwareFile.Length -gt [int64]$status.max_image_bytes) {
    throw "Firmware image is too large: $($firmwareFile.Length) > $($status.max_image_bytes) bytes."
}

$md5 = (Get-FileHash -Algorithm MD5 -LiteralPath $firmwareFile.FullName).Hash.ToLowerInvariant()
$headers = @{
    'X-OTA-Key' = $Token
    'X-Firmware-MD5' = $md5
}

$beforePartition = $status.running_partition
$response = Invoke-RestMethod -Method Post -Uri "$baseUri/api/ota" `
    -Headers $headers -ContentType 'application/octet-stream' `
    -InFile $firmwareFile.FullName -TimeoutSec $UploadTimeoutSeconds

if (-not $response.success) {
    throw 'The bridge did not accept the firmware image.'
}

$deadline = (Get-Date).AddMinutes(3)
$sawOffline = $false
do {
    Start-Sleep -Seconds 2
    try {
        $current = Invoke-RestMethod -Uri "$baseUri/api/ota" -TimeoutSec 3
        if ($sawOffline -and $current.running_partition -ne $beforePartition) {
            [pscustomobject]@{
                Success = $true
                Address = $Address
                PreviousPartition = $beforePartition
                RunningPartition = $current.running_partition
                FirmwareVersion = $current.firmware_version
            }
            exit 0
        }
    } catch {
        $sawOffline = $true
    }
} while ((Get-Date) -lt $deadline)

throw 'OTA upload completed, but the reboot/partition switch could not be confirmed within three minutes.'
