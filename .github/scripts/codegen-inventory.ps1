#!/usr/bin/env pwsh
# Failure-only diagnostic for the yip-app build. The MIDL and cppwinrt outputs
# are the part of that build annotations cannot otherwise show, so report what
# codegen wrote and who defines the module entry points.

$ErrorActionPreference = 'Continue'
$dir = 'yip-app/Generated Files'

if (-not (Test-Path $dir)) {
    Write-Host "no '$dir' — codegen produced nothing"
    exit 1
}

Write-Host '--- generated sources (top level) ---'
Get-ChildItem $dir -File | Select-Object -ExpandProperty Name | Sort-Object | ForEach-Object { Write-Host $_ }

Write-Host '--- module entry points ---'
$symbols = 'WINRT_GetActivationFactory|winrt_get_activation_factory|WINRT_CanUnloadNow|winrt_can_unload_now|wWinMain'
Get-ChildItem $dir -Recurse -Include *.cpp, *.h, *.hpp -File |
    Select-String -Pattern $symbols |
    ForEach-Object { Write-Host "$($_.Filename):$($_.LineNumber): $($_.Line.Trim())" }

# Always non-zero: this step exists to push its findings into an annotation.
exit 1
