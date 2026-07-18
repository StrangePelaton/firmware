$binary = Get-ChildItem -Path "d:\firmware\.pio\build\heltec-v3" -Filter "firmware*.bin" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if ($binary) {
    "✓ BINARY FOUND: $($binary.Name) - Size: $([math]::Round($binary.Length/1024))KB" | Out-File -FilePath "d:\firmware\build_status.txt" -Encoding UTF8
    exit 0
} else {
    $objCount = (Get-ChildItem -Path "d:\firmware\.pio\build\heltec-v3" -Filter "*.o" -Recurse -ErrorAction SilentlyContinue | Measure-Object).Count
    "✗ NO BINARY YET - $objCount .o files present" | Out-File -FilePath "d:\firmware\build_status.txt" -Encoding UTF8
    exit 1
}
