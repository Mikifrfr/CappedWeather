$ErrorActionPreference = 'Stop'
$headers = @{
    'User-Agent' = 'CappedWeather/0.3 live overlay smoke test'
    'Accept' = 'application/geo+json'
}

$alerts = Invoke-RestMethod `
    -Uri 'https://api.weather.gov/alerts/active?status=actual&message_type=alert' `
    -Headers $headers `
    -TimeoutSec 30
if ($alerts.type -ne 'FeatureCollection' -or $null -eq $alerts.features) {
    throw 'NWS active-alert response is not a GeoJSON FeatureCollection.'
}
$polygonWarnings = @($alerts.features | Where-Object { $null -ne $_.geometry }).Count
$zoneOnly = @($alerts.features | Where-Object {
    $null -eq $_.geometry -and @($_.properties.affectedZones).Count -gt 0
})
if ($zoneOnly.Count -gt 0) {
    $zone = Invoke-RestMethod -Uri $zoneOnly[0].properties.affectedZones[0] -Headers $headers -TimeoutSec 30
    if ($null -eq $zone.geometry -or $zone.geometry.type -notmatch 'Polygon') {
        throw 'NWS affected-zone response does not contain polygon geometry.'
    }
}

$reportCounts = @{}
foreach ($kind in @('torn', 'hail', 'wind')) {
    $response = Invoke-WebRequest `
        -Uri "https://www.spc.noaa.gov/climo/reports/today_$kind.csv" `
        -Headers @{ 'User-Agent' = $headers['User-Agent'] } `
        -UseBasicParsing `
        -TimeoutSec 30
    $lines = @($response.Content -split "`r?`n" | Where-Object { $_ })
    if ($lines.Count -eq 0 -or $lines[0] -notmatch '(^|,)Lat(,|$)' -or
        $lines[0] -notmatch '(^|,)Lon(,|$)') {
        throw "SPC $kind report response does not contain Lat/Lon CSV columns."
    }
    $reportCounts[$kind] = [Math]::Max(0, $lines.Count - 1)
}

foreach ($kind in @('torn', 'hail', 'wind')) {
    $response = Invoke-WebRequest -Uri "https://www.spc.noaa.gov/climo/reports/yesterday_$kind.csv" `
        -Headers @{ 'User-Agent' = $headers['User-Agent'] } -UseBasicParsing -TimeoutSec 30
    if ($response.Content -notmatch '(^|,)Lat(,|$)' -or $response.Content -notmatch '(^|,)Lon(,|$)') {
        throw "SPC yesterday $kind report response does not contain Lat/Lon CSV columns."
    }
}

"Live overlay smoke test passed: $polygonWarnings polygon + $($zoneOnly.Count) zone-only warnings; " +
    "$($reportCounts.torn) tornado, $($reportCounts.hail) hail, $($reportCounts.wind) wind reports."
