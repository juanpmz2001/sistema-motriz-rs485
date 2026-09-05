<#
.SYNOPSIS
  Prepares and, with an explicit local confirmation, installs the reviewed Rafa
  SoftAP Web Joystick B47 OTA image.

.DESCRIPTION
  Run only after manually joining the laptop to RAFA-CONTROL.  This script is
  deliberately specific to the B47 experimental image: it refuses a different
  binary, requires the active RAFA-CONTROL / 192.168.4.0/24 connection, performs
  read-only preflight plus OTA check/download_test, and requires a typed local
  confirmation before the update action.  It never sends ARM, motion, enable,
  parameter, fault-clear, or STOP commands.

  It uses BOTFARMS_MAINT_TOKEN and BOTFARMS_OTA_TOKEN from the current process
  when available.  Otherwise it prompts without echoing either value and removes
  those process-local values before exiting.  No token is written to disk.
#>

[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$ValidateOnly,
    [int]$ServerPort = 8080
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ExpectedSsid = 'RAFA-CONTROL'
$ExpectedTarget = '192.168.4.1'
$ExpectedBuild = 47
$ExpectedSha256 = 'BB022796746F1635B7592C8C3C53C781E5224EC8EC2BDC4588E9AF06F45D8EC7'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Binary = Join-Path $RepoRoot 'ota_release\staged\sistema-motriz-rs485-v1.0.0-b47.bin'
$ReleaseDirectory = Join-Path $RepoRoot 'ota_release\b47-softap'
$PrepareRelease = Join-Path $RepoRoot 'tools\ota_prepare_release.py'
$Announce = Join-Path $RepoRoot 'tools\ota_announce.py'
$LanCtl = Join-Path $RepoRoot 'tools\esp_lanctl.py'

function Get-Python {
    $command = Get-Command python -ErrorAction SilentlyContinue
    if (-not $command) {
        throw 'Python is required locally. Install/activate it before leaving the network with Internet access.'
    }

    # Resolve the interpreter reported by Python itself instead of relying on
    # the shell's WindowsApps execution alias in child-process invocations.
    $output = @(& $command.Source -c 'import sys; print(sys.executable)' 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Python resolution failed with exit code $exitCode."
    }
    $python = $output |
        ForEach-Object { "$($_)".Trim() } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -Last 1
    if (-not $python -or -not (Test-Path -LiteralPath $python -PathType Leaf)) {
        throw 'Python did not report a usable executable path.'
    }
    return $python
}

function Read-SecretValue {
    param([Parameter(Mandatory = $true)][string]$Name)

    $existing = [Environment]::GetEnvironmentVariable($Name, 'Process')
    if (-not [string]::IsNullOrWhiteSpace($existing)) {
        return $existing
    }

    $secure = Read-Host -Prompt "$Name (hidden; not persisted)" -AsSecureString
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        $value = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
    }
    finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
    }
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "$Name is required."
    }
    return $value
}

function Restore-ProcessEnvironment {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][bool]$WasPresent,
        [AllowNull()][string]$PreviousValue
    )

    if ($WasPresent) {
        [Environment]::SetEnvironmentVariable($Name, $PreviousValue, 'Process')
    }
    else {
        [Environment]::SetEnvironmentVariable($Name, $null, 'Process')
    }
}

function Assert-B47Artifact {
    if ($ServerPort -lt 1 -or $ServerPort -gt 65535) {
        throw 'ServerPort must be in the range 1..65535.'
    }
    if (-not (Test-Path -LiteralPath $Binary -PathType Leaf)) {
        throw "B47 artifact not found: $Binary"
    }

    $versionHeader = Join-Path $RepoRoot 'main\app_version.h'
    $versionText = Get-Content -LiteralPath $versionHeader -Raw
    if ($versionText -notmatch "(?m)^#define\s+FW_BUILD_NUMBER\s+$ExpectedBuild\s*$") {
        throw "This worktree is not the expected B$ExpectedBuild source."
    }

    $actualHash = (Get-FileHash -LiteralPath $Binary -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($actualHash -ne $ExpectedSha256) {
        throw "B47 artifact SHA-256 mismatch. Expected $ExpectedSha256, got $actualHash."
    }
    Write-Host "B47 artifact verified: $actualHash"
}

function Get-SoftApHostIp {
    $profile = Get-NetConnectionProfile | Where-Object { $_.Name -eq $ExpectedSsid } | Select-Object -First 1
    if (-not $profile) {
        throw "Join Wi-Fi '$ExpectedSsid' first; the active connection is not the Rafa SoftAP."
    }

    $candidate = Get-NetIPAddress -InterfaceIndex $profile.InterfaceIndex -AddressFamily IPv4 |
        Where-Object {
            $_.PrefixLength -eq 24 -and
            $_.IPAddress -like '192.168.4.*' -and
            $_.IPAddress -ne $ExpectedTarget -and
            $_.IPAddress -ne '192.168.4.0' -and
            $_.IPAddress -ne '192.168.4.255' -and
            $_.AddressState -eq 'Preferred'
        } |
        Select-Object -First 1
    if (-not $candidate) {
        throw "'$ExpectedSsid' is connected, but this laptop has no preferred DHCP address in 192.168.4.0/24."
    }
    return $candidate.IPAddress
}

function Invoke-LocalPython {
    param(
        [Parameter(Mandatory = $true)][string]$Python,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    Write-Host "`n== $Label =="
    $lines = @(& $Python @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    foreach ($line in $lines) {
        Write-Host $line
    }
    if ($exitCode -ne 0) {
        throw "$Label failed with exit code $exitCode."
    }
    return ($lines -join "`n")
}

function Invoke-Maintenance {
    param(
        [Parameter(Mandatory = $true)][string]$Python,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string[]]$CommandArguments
    )

    return Invoke-LocalPython -Python $Python -Label $Label -Arguments (@($LanCtl) + $CommandArguments)
}

function Get-DirectHttpStatusCode {
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [int]$TimeoutMs = 1000
    )

    # PowerShell's Invoke-WebRequest can involve proxy/WinINet behavior that is
    # unrelated to the direct SoftAP route.  Send a minimal raw HTTP GET over a
    # bounded TCP connection instead.
    $endpoint = [System.Uri]$Uri
    if ($endpoint.Scheme -ne 'http') {
        throw "Expected an HTTP URI, got $Uri."
    }

    $client = [System.Net.Sockets.TcpClient]::new()
    $stream = $null
    $waitHandle = $null
    try {
        $connect = $client.BeginConnect($endpoint.Host, $endpoint.Port, $null, $null)
        $waitHandle = $connect.AsyncWaitHandle
        if (-not $waitHandle.WaitOne($TimeoutMs)) {
            throw "TCP connect timeout after $TimeoutMs ms."
        }
        $client.EndConnect($connect)

        $stream = $client.GetStream()
        $stream.ReadTimeout = $TimeoutMs
        $stream.WriteTimeout = $TimeoutMs
        $hostHeader = if ($endpoint.IsDefaultPort) { $endpoint.Host } else { "$($endpoint.Host):$($endpoint.Port)" }
        $requestText = "GET $($endpoint.PathAndQuery) HTTP/1.1`r`nHost: $hostHeader`r`nConnection: close`r`n`r`n"
        $requestBytes = [System.Text.Encoding]::ASCII.GetBytes($requestText)
        $stream.Write($requestBytes, 0, $requestBytes.Length)
        $stream.Flush()

        $buffer = New-Object byte[] 512
        $read = $stream.Read($buffer, 0, $buffer.Length)
        if ($read -le 0) {
            throw 'HTTP server closed the connection without a response.'
        }
        $statusLine = ([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read).Split("`r`n"))[0]
        if ($statusLine -notmatch '^HTTP/\d\.\d\s+(\d{3})(?:\s|$)') {
            throw "Malformed HTTP response: $statusLine"
        }
        return [int]$Matches[1]
    }
    finally {
        if ($stream) {
            $stream.Dispose()
        }
        if ($waitHandle) {
            $waitHandle.Close()
        }
        if ($client) {
            $client.Dispose()
        }
    }
}

function Get-ListenerDetail {
    param([Parameter(Mandatory = $true)][int]$Port)

    # Get-NetTCPConnection reports an empty result as a CIM error when asked
    # for a port that has no listener.  That is the normal pre-start state, not
    # a failure to inspect the machine.
    $listeners = @(Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue)
    if ($listeners.Count -eq 0) {
        return 'no listener'
    }
    return ($listeners |
        ForEach-Object { "PID $($_.OwningProcess) at $($_.LocalAddress):$($_.LocalPort)" }) -join '; '
}

function Assert-Preflight {
    param([Parameter(Mandatory = $true)][string]$Python)

    $version = Invoke-Maintenance -Python $Python -Label 'Read-only VERSION' -CommandArguments @('version', '--host', $ExpectedTarget)
    $profile = Invoke-Maintenance -Python $Python -Label 'Read-only PROFILE_STATUS' -CommandArguments @('command', '--host', $ExpectedTarget, 'PROFILE_STATUS')
    $platform = Invoke-Maintenance -Python $Python -Label 'Read-only PLATFORM_STATUS' -CommandArguments @('platform-status', '--host', $ExpectedTarget)
    $safety = Invoke-Maintenance -Python $Python -Label 'Read-only SAFETY_STATUS' -CommandArguments @('safety-status', '--host', $ExpectedTarget)
    [void](Invoke-Maintenance -Python $Python -Label 'Read-only WIFI_STATUS' -CommandArguments @('wifi-status', '--host', $ExpectedTarget))
    [void](Invoke-Maintenance -Python $Python -Label 'Read-only OTA_CONFIG' -CommandArguments @('ota-status', '--host', $ExpectedTarget))

    if ($profile -notmatch 'NAME:rafa_softap_web_joystick_experimental') {
        throw 'Refusing OTA: target profile is not rafa_softap_web_joystick_experimental.'
    }
    if ($platform -notmatch 'STATE:SAFE_IDLE' -or $platform -notmatch 'SAFE_FOR_OTA:1') {
        throw 'Refusing OTA: PLATFORM_STATUS is not SAFE_IDLE and SAFE_FOR_OTA:1.'
    }
    if ($safety -notmatch 'TASK:RUNNING' -or $safety -match 'MOTOR_FAULT:1') {
        throw 'Refusing OTA: safety task is not running or reports a motor fault.'
    }

    return $version
}

function Start-ReleaseServer {
    param(
        [Parameter(Mandatory = $true)][string]$Python,
        [Parameter(Mandatory = $true)][string]$HostIp
    )

    $stdout = Join-Path $ReleaseDirectory 'http-server.stdout.log'
    $stderr = Join-Path $ReleaseDirectory 'http-server.stderr.log'
    $existingListener = Get-ListenerDetail -Port $ServerPort
    if ($existingListener -ne 'no listener') {
        throw "Refusing to start the local OTA server: TCP port $ServerPort is already in use or could not be inspected ($existingListener)."
    }
    $arguments = '-m http.server {0} --bind 0.0.0.0 --directory "{1}"' -f $ServerPort, $ReleaseDirectory
    $startArguments = @{
        FilePath = $Python
        ArgumentList = $arguments
        WindowStyle = 'Hidden'
        RedirectStandardOutput = $stdout
        RedirectStandardError = $stderr
        PassThru = $true
    }
    $process = Start-Process @startArguments

    # Start-Process returning does not mean that Python has already bound the
    # socket.  Wait for its own listener and a direct raw-HTTP loopback response.
    # The ESP's later authenticated check/download_test is the final proof of
    # the Rafa-to-laptop HTTP path; a Windows self-probe of the SoftAP address
    # is useful diagnostic evidence but must not reject that real test early.
    $loopbackUrl = "http://127.0.0.1`:$ServerPort/api/firmware/latest"
    $softApUrl = "http://$HostIp`:$ServerPort/api/firmware/latest"
    $deadline = (Get-Date).AddSeconds(10)
    $loopbackError = 'not attempted'
    $softApError = 'not attempted'
    $loopbackReady = $false
    $softApReady = $false

    while ((Get-Date) -lt $deadline) {
        if ($process.HasExited) {
            $stderrText = Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue
            throw "The local HTTP server exited before it was ready (PID $($process.Id)). stderr: $stderrText"
        }

        $loopbackReady = $false
        try {
            $loopbackStatus = Get-DirectHttpStatusCode -Uri $loopbackUrl
            $loopbackReady = $loopbackStatus -eq 200
            if (-not $loopbackReady) {
                $loopbackError = "HTTP $loopbackStatus"
            }
        }
        catch {
            $loopbackError = $_.Exception.Message
        }

        $softApReady = $false
        try {
            $softApStatus = Get-DirectHttpStatusCode -Uri $softApUrl
            $softApReady = $softApStatus -eq 200
            if (-not $softApReady) {
                $softApError = "HTTP $softApStatus"
            }
        }
        catch {
            $softApError = $_.Exception.Message
        }

        $listenerDetail = Get-ListenerDetail -Port $ServerPort
        $serverOwnsListener = $listenerDetail -match "PID $($process.Id) at "
        if ($loopbackReady -and $serverOwnsListener) {
            if ($softApReady) {
                Write-Host "Local OTA manifest ready: $softApUrl"
            }
            else {
                Write-Host "Local OTA server is ready; SoftAP self-probe is diagnostic only and did not respond: $softApError"
            }
            return $process
        }
        Start-Sleep -Milliseconds 250
    }

    $stderrText = Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue
    $listenerDetail = Get-ListenerDetail -Port $ServerPort
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    throw "The local OTA server did not become ready within 10 seconds (PID $($process.Id)). loopback=$loopbackError; softap diagnostic=$softApError; listener=$listenerDetail; stderr=$stderrText"
}

function Invoke-OtaAction {
    param(
        [Parameter(Mandatory = $true)][string]$Python,
        [Parameter(Mandatory = $true)][ValidateSet('check', 'download_test', 'update')][string]$Action
    )

    $responseWindowSeconds = switch ($Action) {
        'check' { 8 }
        'download_test' { 60 }
        'update' { 60 }
    }
    [void](Invoke-LocalPython -Python $Python -Label "OTA $Action" -Arguments @(
        $Announce,
        '--target', $ExpectedTarget,
        '--server-port', "$ServerPort",
        '--manifest', '/api/firmware/latest',
        '--action', $Action,
        '--timeout', "$responseWindowSeconds",
        '--count', '1',
        '--interval', "$responseWindowSeconds",
        '--stop-after-first-response'
    ))
}

function Wait-ForB47 {
    param([Parameter(Mandatory = $true)][string]$Python)

    $deadline = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        try {
            $version = Invoke-Maintenance -Python $Python -Label 'Post-reboot VERSION' -CommandArguments @('version', '--host', $ExpectedTarget)
            if ($version -match 'BUILD_NUMBER:47' -and $version -match 'GIT_SHA:111de2227ba0f508266828dac1d1da400243619c') {
                [void](Invoke-Maintenance -Python $Python -Label 'Post-reboot PROFILE_STATUS' -CommandArguments @('command', '--host', $ExpectedTarget, 'PROFILE_STATUS'))
                [void](Invoke-Maintenance -Python $Python -Label 'Post-reboot PLATFORM_STATUS' -CommandArguments @('platform-status', '--host', $ExpectedTarget))
                [void](Invoke-Maintenance -Python $Python -Label 'Post-reboot SAFETY_STATUS' -CommandArguments @('safety-status', '--host', $ExpectedTarget))
                [void](Invoke-Maintenance -Python $Python -Label 'Post-reboot WIFI_STATUS' -CommandArguments @('wifi-status', '--host', $ExpectedTarget))
                Write-Host "`nB47 installation verified. Rafa remains unarmed."
                return
            }
        }
        catch {
            Write-Host 'Waiting for Rafa reboot and Maintenance LAN...'
        }
    }
    throw 'OTA update was acknowledged, but B47 could not be verified within 90 seconds. Do not assume installation succeeded.'
}

Assert-B47Artifact
if ($ValidateOnly) {
    Write-Host 'Local B47 OTA artifact validation: PASS'
    return
}

$hostIp = Get-SoftApHostIp
Write-Host "Using Rafa SoftAP: laptop=$hostIp target=$ExpectedTarget"
$python = Get-Python

$hadMaintToken = Test-Path Env:BOTFARMS_MAINT_TOKEN
$previousMaintToken = $env:BOTFARMS_MAINT_TOKEN
$hadOtaToken = Test-Path Env:BOTFARMS_OTA_TOKEN
$previousOtaToken = $env:BOTFARMS_OTA_TOKEN
$httpServer = $null

try {
    $env:BOTFARMS_MAINT_TOKEN = Read-SecretValue -Name 'BOTFARMS_MAINT_TOKEN'
    $env:BOTFARMS_OTA_TOKEN = Read-SecretValue -Name 'BOTFARMS_OTA_TOKEN'

    [void](Assert-Preflight -Python $python)
    [void](Invoke-LocalPython -Python $python -Label 'Prepare B47 local OTA release' -Arguments @(
        $PrepareRelease,
        '--host', $hostIp,
        '--port', "$ServerPort",
        '--binary', $Binary,
        '--output', $ReleaseDirectory
    ))
    $httpServer = Start-ReleaseServer -Python $python -HostIp $hostIp
    Invoke-OtaAction -Python $python -Action 'check'
    Invoke-OtaAction -Python $python -Action 'download_test'

    if (-not $Install) {
        Write-Host "`nB47 check and download test passed. Run this same script with -Install only when ready to request the rebooting OTA update."
        return
    }

    $confirmation = Read-Host 'Type INSTALL B47 to request the OTA update (anything else cancels)'
    if ($confirmation -ne 'INSTALL B47') {
        Write-Host 'OTA update cancelled; check and download_test already completed.'
        return
    }
    Invoke-OtaAction -Python $python -Action 'update'
    Wait-ForB47 -Python $python
}
finally {
    if ($httpServer -and -not $httpServer.HasExited) {
        Stop-Process -Id $httpServer.Id -Force -ErrorAction SilentlyContinue
    }
    Restore-ProcessEnvironment -Name 'BOTFARMS_MAINT_TOKEN' -WasPresent $hadMaintToken -PreviousValue $previousMaintToken
    Restore-ProcessEnvironment -Name 'BOTFARMS_OTA_TOKEN' -WasPresent $hadOtaToken -PreviousValue $previousOtaToken
}
