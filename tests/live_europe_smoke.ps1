param(
    [string]$OdimCode = 'nohur',
    [string]$CountryCode = 'NO'
)

$ErrorActionPreference = 'Stop'
$headers = @{ 'User-Agent' = 'CappedWeather/0.4 live Europe smoke test' }
$bucket = 'https://s3.waw3-1.cloudferro.com/openradar-24h'

# Rolling 24 h cache: try today, then yesterday (UTC date rollover).
# NOTE: build the prefix manually; ToString('yyyy/MM/dd') localizes the
# slashes to the OS date separator on some machines.
function Date-Prefix([DateTime]$day) {
    return "$($day.Year)/$($day.Month.ToString('00'))/$($day.Day.ToString('00'))"
}
$dates = @(
    (Date-Prefix ([DateTime]::UtcNow)),
    (Date-Prefix ([DateTime]::UtcNow.AddDays(-1)))
)
$objects = @()
foreach ($date in $dates) {
    $prefix = "$date/$CountryCode/$OdimCode/PVOL/"
    $query = "?list-type=2&prefix=$([Uri]::EscapeDataString($prefix))&max-keys=100"
    [xml]$page = (Invoke-WebRequest -Uri ($bucket + $query) -Headers $headers -UseBasicParsing -TimeoutSec 30).Content
    $objects += @($page.ListBucketResult.Contents | Where-Object { $_.Key -like '*@DBZH.h5' -and [UInt64]$_.Size -gt 0 })
    if ($objects.Count -gt 0) { break }
}
if ($objects.Count -eq 0) {
    throw "No OPERA DBZH volumes found for $OdimCode on either date."
}

$latest = $objects | Sort-Object Key -Descending | Select-Object -First 1
if ($latest.Key -notmatch '@(\d{8}T\d{4})@') {
    throw "OPERA key does not carry a timestamp: $($latest.Key)."
}

# First 8 bytes must be the HDF5 signature (89 48 44 46 0D 0A 1A 0A).
$path = Join-Path $env:TEMP "cappedweather-live-europe-$OdimCode.h5"
Invoke-WebRequest -Uri ($bucket + '/' + $latest.Key) -Headers $headers -OutFile $path -UseBasicParsing -TimeoutSec 120
$bytes = [IO.File]::ReadAllBytes($path)
if ($bytes.Length -lt 8 -or $bytes[0] -ne 0x89 -or $bytes[1] -ne 0x48 -or $bytes[2] -ne 0x44 -or $bytes[3] -ne 0x46) {
    throw "Latest object is not an HDF5 container: $($latest.Key)."
}

"Live Europe smoke test passed: $($objects.Count) DBZH objects; $($latest.Key)."
