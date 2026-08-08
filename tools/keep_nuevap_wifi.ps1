$createdNew = $false
$mutex = [Threading.Mutex]::new(
  $true,
  'Local\NuevaPataWifiWatch',
  [ref]$createdNew
)

if (-not $createdNew) {
  exit 0
}

try {
  $failedHealthChecks = 0

  while ($true) {
    $interfaces = (& netsh.exe wlan show interfaces | Out-String)
    $match = [regex]::Match(
      $interfaces,
      '(?ms)^\s*Nombre\s+:\s*Wi-Fi 2\s*$.*?(?=^\s*Nombre\s+:|\z)'
    )
    $onNuevaPata =
      $match.Success -and
      $match.Value -match '(?m)^\s*Estado\s+:\s*conectado\s*$' -and
      $match.Value -match '(?m)^\s*SSID\s+:\s*NuevaPata\s*$'

    if (-not $onNuevaPata) {
      & netsh.exe wlan connect `
        name='NuevaPata' `
        ssid='NuevaPata' `
        interface='Wi-Fi 2' | Out-Null
      $failedHealthChecks = 0
    }
    else {
      & ping.exe -n 1 -w 800 192.168.4.1 | Out-Null
      if ($LASTEXITCODE -eq 0) {
        $failedHealthChecks = 0
      }
      else {
        $failedHealthChecks++
      }

      # El Realtek puede quedar asociado al SSID pero sin tráfico ARP/IP.
      # Tres fallos consecutivos fuerzan una reasociación limpia.
      if ($failedHealthChecks -ge 3) {
        & netsh.exe wlan disconnect interface='Wi-Fi 2' | Out-Null
        Start-Sleep -Seconds 1
        & netsh.exe wlan connect `
          name='NuevaPata' `
          ssid='NuevaPata' `
          interface='Wi-Fi 2' | Out-Null
        $failedHealthChecks = 0
      }
    }

    Start-Sleep -Seconds 2
  }
}
finally {
  $mutex.ReleaseMutex()
  $mutex.Dispose()
}
