$ErrorActionPreference = 'Continue'

$failed = @()
foreach ($wat in Get-ChildItem -Path $PSScriptRoot -Filter '*.wat') {
    $wasm = [IO.Path]::ChangeExtension($wat.FullName, '.wasm')
    & wasm-tools parse $wat.FullName -o $wasm
    if ($LASTEXITCODE -ne 0) {
        $failed += $wat.Name
        continue
    }
    Write-Host "Built $([IO.Path]::GetFileName($wasm))"
}

if ($failed.Count -gt 0) {
    Write-Error "wasm-tools failed on: $($failed -join ', ')"
    exit 1
}
