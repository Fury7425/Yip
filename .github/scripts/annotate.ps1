#!/usr/bin/env pwsh
# Run a gate command and, on failure, republish its output as GitHub Actions
# error annotations.
#
# Raw job logs on this repository need a signed-in session to read, while check
# annotations are served by the public check-runs API. Mirroring the failure
# into annotations keeps a red gate diagnosable from the API alone.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$Command,
    [int]$ChunkChars = 6000,
    [int]$MaxChunks = 8,
    # Keep the last chunk-worth of output instead of the first. Both cargo and
    # MSBuild put their diagnostics after a long preamble of progress lines.
    [switch]$Head
)

$ErrorActionPreference = 'Continue'
$env:CARGO_TERM_COLOR = 'never'

$global:LASTEXITCODE = 0
$raw = & { Invoke-Expression $Command } 2>&1
$code = $LASTEXITCODE
if ($null -eq $code) { $code = 0 }

# A command that never launched leaves LASTEXITCODE at whatever it was, so a
# missing tool would report a green gate that checked nothing.
$missing = @($raw | Where-Object {
        $_ -is [System.Management.Automation.ErrorRecord] -and
        $_.Exception -is [System.Management.Automation.CommandNotFoundException]
    }).Count
if ($missing -gt 0 -and $code -eq 0) { $code = 127 }

$lines = $raw | ForEach-Object { $_.ToString() }

$text = ($lines -join "`n")
if ($text) { Write-Host $text }

if ($code -eq 0) { exit 0 }

# Strip ANSI, normalise newlines, drop cargo/MSBuild progress chatter, then
# escape for the workflow-command grammar. The annotation budget is small; a
# hundred "Downloaded ..." lines would push the real diagnostics out of it.
$noise = '^\s*(Downloading|Downloaded|Compiling|Checking|Updating|Locking|Adding|Fresh|Installing|Blocking|Ignored) '
$kept = ((($text -replace "`e\[[0-9;?]*[a-zA-Z]", '') -replace "`r", '') -split "`n") |
    Where-Object { $_ -notmatch $noise }
$clean = ($kept -join "`n").Trim()
if (-not $clean) { $clean = "$Title failed with exit code $code (no output captured)." }

$budget = $ChunkChars * $MaxChunks
if (-not $Head -and $clean.Length -gt $budget) {
    $clean = $clean.Substring($clean.Length - $budget)
}

$chunks = @()
for ($i = 0; $i -lt $clean.Length -and $chunks.Count -lt $MaxChunks; $i += $ChunkChars) {
    $len = [Math]::Min($ChunkChars, $clean.Length - $i)
    $chunks += $clean.Substring($i, $len)
}

$total = $chunks.Count
for ($i = 0; $i -lt $total; $i++) {
    $encoded = $chunks[$i].Replace('%', '%25').Replace("`n", '%0A')
    Write-Host "::error title=$Title ($($i + 1)/$total)::$encoded"
}

exit $code
