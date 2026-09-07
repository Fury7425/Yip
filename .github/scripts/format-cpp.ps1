#!/usr/bin/env pwsh
# clang-format gate for yip-app/ and vst-host/. Generated and build outputs are
# excluded: MIDL and the WinUI codegen emit files nobody hand-edits.

$ErrorActionPreference = 'Continue'

# A missing clang-format would otherwise leave $LASTEXITCODE untouched and the
# gate would report green without having checked anything.
$tool = Get-Command clang-format -ErrorAction SilentlyContinue
if (-not $tool) {
    Write-Host 'clang-format was not found on PATH.'
    exit 127
}
Write-Host "using $($tool.Source)"
& clang-format --version

$files = Get-ChildItem -Recurse -Include *.cpp, *.h -Path yip-app, vst-host |
    Where-Object { $_.FullName -notmatch '\build\' -and $_.FullName -notmatch '\Generated Files\' }

if ($files.Count -eq 0) {
    Write-Host 'No C++ files to format-check.'
    exit 0
}

& clang-format --dry-run -Werror --style=file $files.FullName
exit $LASTEXITCODE
