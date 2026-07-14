param(
    [string]$RadarId = 'KTLX'
)

$ErrorActionPreference = 'Stop'
$headers = @{ 'User-Agent' = 'CappedWeather/0.3 live smoke test' }

$catalog = Invoke-RestMethod -Uri 'https://api.weather.gov/radar/stations' -Headers $headers -TimeoutSec 30
$wsr88d = @($catalog.features | Where-Object { $_.properties.stationType -eq 'WSR-88D' })
$tdwr = @($catalog.features | Where-Object { $_.properties.stationType -eq 'TDWR' })
if ($wsr88d.Count -lt 150) {
    throw "Expected at least 150 official WSR-88D stations, received $($wsr88d.Count)."
}
if ($tdwr.Count -lt 45) {
    throw "Expected at least 45 official TDWR stations, received $($tdwr.Count)."
}
if (-not @($catalog.features | Where-Object { $_.properties.id -eq $RadarId })) {
    throw "$RadarId is not present in the official NWS radar catalog."
}

$token = $null
$objects = @()
do {
    $query = "?list-type=2&prefix=$([Uri]::EscapeDataString($RadarId + '/'))&max-keys=1000"
    if ($token) {
        $query += "&continuation-token=$([Uri]::EscapeDataString($token))"
    }
    [xml]$page = (Invoke-WebRequest -Uri ("https://unidata-nexrad-level2-chunks.s3.amazonaws.com/" + $query) -Headers $headers -UseBasicParsing -TimeoutSec 30).Content
    $objects += @($page.ListBucketResult.Contents | Where-Object { [UInt64]$_.Size -gt 0 })
    $token = [string]$page.ListBucketResult.NextContinuationToken
} while ($page.ListBucketResult.IsTruncated -eq 'true' -and $token)

$latest = $objects | Sort-Object { [DateTime]$_.LastModified } -Descending | Select-Object -First 1
if (-not $latest) {
    throw "No real Level II object was found for $RadarId."
}

$path = Join-Path $env:TEMP "cappedweather-live-$RadarId.bin"
Invoke-WebRequest -Uri ("https://unidata-nexrad-level2-chunks.s3.amazonaws.com/" + $latest.Key) -Headers $headers -OutFile $path -UseBasicParsing -TimeoutSec 60
$bytes = [IO.File]::ReadAllBytes($path)
if ($bytes.Length -lt 7 -or $bytes[4] -ne 0x42 -or $bytes[5] -ne 0x5A -or $bytes[6] -ne 0x68) {
    throw "Latest object is not a bzip2 Level II chunk."
}

"Live smoke test passed: $($wsr88d.Count) WSR-88D + $($tdwr.Count) TDWR stations; $($latest.Key); $($bytes.Length) bytes."
