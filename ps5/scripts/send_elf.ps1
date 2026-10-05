<#
.SYNOPSIS
  Send an ELF payload to the PS5 ELF loader (elfldr) over TCP. Windows replacement for:
      nc -q0 $PS5_HOST 9021 < payload.elf

.EXAMPLE
  .\send_elf.ps1 -Ps5Host 192.168.1.50 -Elf ..\baseline\hello_world.elf
  .\send_elf.ps1 -Ps5Host 192.168.1.50 -Elf ..\baseline\hello_world.elf -CheckOnly   # only test that the port is open
#>
param(
    [Parameter(Mandatory = $true)][string]$Ps5Host,
    [string]$Elf,
    [int]$Port = 9021,
    [int]$ListenSeconds = 5,
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'

if (-not $CheckOnly) {
    if (-not $Elf) { throw "Specify -Elf <path> (or use -CheckOnly)." }
    if (-not (Test-Path $Elf)) { throw "ELF not found: $Elf" }
}

$client = New-Object System.Net.Sockets.TcpClient
try {
    $async = $client.BeginConnect($Ps5Host, $Port, $null, $null)
    if (-not $async.AsyncWaitHandle.WaitOne(4000)) {
        throw "Timeout: ${Ps5Host}:${Port} not reachable. Is the loader running, and is the IP correct?"
    }
    $client.EndConnect($async)
    Write-Host "Connected to ${Ps5Host}:${Port}"

    if ($CheckOnly) { Write-Host "Port is open. Nothing sent."; return }

    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $Elf))
    $stream = $client.GetStream()
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    # Half-close so the loader sees end-of-file (same as nc -q0).
    $client.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send)
    Write-Host ("Sent {0} bytes from {1}" -f $bytes.Length, $Elf)

    # Print anything the payload writes to stdout/stderr (elfldr forwards it on the socket).
    $client.ReceiveTimeout = 1000
    $buf = New-Object byte[] 4096
    $deadline = (Get-Date).AddSeconds($ListenSeconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -le 0) { break }
            Write-Host -NoNewline ([Text.Encoding]::UTF8.GetString($buf, 0, $n))
        } catch { }
    }
    Write-Host "`nDone."
}
finally {
    $client.Close()
}
