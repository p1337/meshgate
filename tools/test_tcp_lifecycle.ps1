[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$TargetHost,

    [ValidateRange(1, 65535)]
    [int]$Port = 4403,

    [ValidateRange(1, 100000)]
    [int]$Cycles = 1000,

    [ValidateRange(1, 1000)]
    [int]$BatchSize = 50,

    [ValidateRange(100, 30000)]
    [int]$ConnectTimeoutMs = 3000,

    [ValidateRange(5, 600)]
    [int]$CleanupSeconds = 60,

    [ValidateRange(0, 65536)]
    [int]$AllowedHeapDriftBytes = 4096,

    [string]$StatusUrl,

    [string]$OutputPath,

    [switch]$ConfirmStress
)

$ErrorActionPreference = 'Stop'

function Test-PrivateAddress {
    param([System.Net.IPAddress]$Address)

    if ($Address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) {
        return $Address.Equals([System.Net.IPAddress]::IPv6Loopback)
    }

    $bytes = $Address.GetAddressBytes()
    return (
        $bytes[0] -eq 10 -or
        ($bytes[0] -eq 172 -and $bytes[1] -ge 16 -and $bytes[1] -le 31) -or
        ($bytes[0] -eq 192 -and $bytes[1] -eq 168) -or
        $bytes[0] -eq 127
    )
}

function Assert-LanTarget {
    param([string]$Name)

    $addresses = [System.Net.Dns]::GetHostAddresses($Name)
    if ($addresses.Count -eq 0) {
        throw "Target '$Name' did not resolve."
    }
    $nonPrivate = @($addresses | Where-Object { -not (Test-PrivateAddress $_) })
    if ($nonPrivate.Count -gt 0) {
        throw "Refusing to stress a non-LAN target ($($nonPrivate -join ', ')). Run this test only against the device at home on a private LAN address."
    }
}

function Get-StatusSnapshot {
    param([string]$Uri)

    $status = Invoke-RestMethod -Uri $Uri -TimeoutSec 15
    $tcp = $status.tcp
    $drops = 0
    foreach ($queue in @('tcp_to_ble', 'ble_to_tcp', 'ble_to_http', 'mqtt_to_ble')) {
        $drops += [long]$status.queues.$queue.dropped
    }

    [pscustomobject]@{
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        uptime_seconds = [long]$status.uptime_seconds
        heap_free_bytes = [long]$status.heap.free_bytes
        heap_min_free_bytes = [long]$status.heap.min_free_bytes
        largest_free_block_bytes = [long]$status.heap.max_alloc_bytes
        connected_clients = [long]$tcp.connected_clients
        accepted_total = if ($null -ne $tcp.connections_accepted_total) { [long]$tcp.connections_accepted_total } else { $null }
        disconnected_total = if ($null -ne $tcp.connections_disconnected_total) { [long]$tcp.connections_disconnected_total } else { $null }
        rejected_total = if ($null -ne $tcp.connections_rejected_total) { [long]$tcp.connections_rejected_total } else { $null }
        context_alloc_failures_total = if ($null -ne $tcp.context_alloc_failures_total) { [long]$tcp.context_alloc_failures_total } else { $null }
        queue_drops_total = $drops
        ble_connected = [bool]$status.ble.connected
        mqtt_state = [string]$status.mqtt.state
    }
}

function Get-BestValue {
    param([object[]]$Samples, [string]$Property)
    return [long](($Samples | Measure-Object -Property $Property -Maximum).Maximum)
}

if (-not $ConfirmStress) {
    throw 'Stress test not started. Re-run with -ConfirmStress after verifying that MeshGate is at home on the local LAN.'
}

Assert-LanTarget $TargetHost

if ([string]::IsNullOrWhiteSpace($StatusUrl)) {
    $StatusUrl = "http://$TargetHost/status"
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
    $OutputPath = Join-Path ([System.IO.Path]::GetTempPath()) "meshgate-tcp-lifecycle-$stamp.json"
}

Write-Host "Collecting baseline from $StatusUrl ..."
$before = @()
1..5 | ForEach-Object {
    $before += Get-StatusSnapshot $StatusUrl
    Start-Sleep -Seconds 1
}

$cycleSamples = @()
$failures = [System.Collections.Generic.List[object]]::new()
$succeeded = 0
$started = [Diagnostics.Stopwatch]::StartNew()

for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connectTask = $client.ConnectAsync($TargetHost, $Port)
        if (-not $connectTask.Wait($ConnectTimeoutMs)) {
            throw "connect timeout after $ConnectTimeoutMs ms"
        }
        if (-not $client.Connected) {
            throw 'socket did not enter connected state'
        }
        $succeeded++
    } catch {
        $failures.Add([pscustomobject]@{
            cycle = $cycle
            error = $_.Exception.GetBaseException().Message
        })
    } finally {
        $client.Dispose()
    }

    if (($cycle % $BatchSize) -eq 0 -or $cycle -eq $Cycles) {
        Start-Sleep -Milliseconds 500
        $sample = Get-StatusSnapshot $StatusUrl
        $cycleSamples += $sample
        Write-Host ("{0}/{1}: ok={2}, failed={3}, heap={4}, largest={5}, active={6}" -f `
            $cycle, $Cycles, $succeeded, $failures.Count, $sample.heap_free_bytes, `
            $sample.largest_free_block_bytes, $sample.connected_clients)
    }
}
$started.Stop()

Write-Host "Waiting $CleanupSeconds seconds for lwIP cleanup ..."
$after = @()
$remaining = $CleanupSeconds
while ($remaining -gt 0) {
    $sleepFor = [Math]::Min(5, $remaining)
    Start-Sleep -Seconds $sleepFor
    $remaining -= $sleepFor
    $after += Get-StatusSnapshot $StatusUrl
}

$baselineFree = Get-BestValue $before 'heap_free_bytes'
$baselineLargest = Get-BestValue $before 'largest_free_block_bytes'
$recoveredFree = Get-BestValue $after 'heap_free_bytes'
$recoveredLargest = Get-BestValue $after 'largest_free_block_bytes'
$heapDrift = [Math]::Max(0, $baselineFree - $recoveredFree)
$largestBlockDrift = [Math]::Max(0, $baselineLargest - $recoveredLargest)
$first = $before[0]
$last = $after[-1]

$hasLifecycleCounters = $null -ne $first.accepted_total -and $null -ne $last.accepted_total
$acceptedDelta = if ($hasLifecycleCounters) { $last.accepted_total - $first.accepted_total } else { $null }
$disconnectedDelta = if ($hasLifecycleCounters) { $last.disconnected_total - $first.disconnected_total } else { $null }
$allocationFailureDelta = if ($hasLifecycleCounters) { $last.context_alloc_failures_total - $first.context_alloc_failures_total } else { $null }

$checks = [ordered]@{
    all_connections_succeeded = ($succeeded -eq $Cycles -and $failures.Count -eq 0)
    no_restart = ($last.uptime_seconds -ge $first.uptime_seconds)
    no_active_clients_after_cleanup = ($last.connected_clients -eq 0)
    heap_returned_to_range = ($heapDrift -le $AllowedHeapDriftBytes)
    largest_block_returned_to_range = ($largestBlockDrift -le $AllowedHeapDriftBytes)
    no_new_queue_drops = ($last.queue_drops_total -eq $first.queue_drops_total)
    ble_remained_connected = [bool]$last.ble_connected
    lifecycle_balanced = (-not $hasLifecycleCounters -or ($acceptedDelta -eq $Cycles -and $disconnectedDelta -eq $Cycles))
    no_context_allocation_failures = (-not $hasLifecycleCounters -or $allocationFailureDelta -eq 0)
}

$passed = -not ($checks.Values -contains $false)
$result = [ordered]@{
    schema_version = 1
    passed = $passed
    target = [ordered]@{ host = $TargetHost; port = $Port; status_url = $StatusUrl }
    parameters = [ordered]@{
        cycles = $Cycles
        batch_size = $BatchSize
        connect_timeout_ms = $ConnectTimeoutMs
        cleanup_seconds = $CleanupSeconds
        allowed_heap_drift_bytes = $AllowedHeapDriftBytes
    }
    summary = [ordered]@{
        succeeded = $succeeded
        failed = $failures.Count
        elapsed_seconds = [Math]::Round($started.Elapsed.TotalSeconds, 3)
        baseline_heap_free_bytes = $baselineFree
        recovered_heap_free_bytes = $recoveredFree
        heap_drift_bytes = $heapDrift
        baseline_largest_block_bytes = $baselineLargest
        recovered_largest_block_bytes = $recoveredLargest
        largest_block_drift_bytes = $largestBlockDrift
        accepted_delta = $acceptedDelta
        disconnected_delta = $disconnectedDelta
        context_allocation_failure_delta = $allocationFailureDelta
    }
    checks = $checks
    failures = $failures
    before = $before
    during = $cycleSamples
    after = $after
}

$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding utf8
Write-Host "Result: $(if ($passed) { 'PASS' } else { 'FAIL' })"
Write-Host "Evidence: $OutputPath"

if (-not $passed) {
    exit 1
}
