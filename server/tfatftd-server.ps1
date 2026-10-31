# tfatftd.xyz multiplayer server (v0.01) - a tiny relay that runs on your own PC.
# Every player's browser sends its car position ~15 times a second; the server passes it to everyone else.
# Plain PowerShell, no exe (nothing for Smart App Control to block).
#   local test:  ws://localhost:8787
#   friends:     the https address Tailscale Funnel prints (the .bat sets that up)
param([int]$Port = 8787)
$ErrorActionPreference = 'Stop'

$clients = [hashtable]::Synchronized(@{})   # id -> @{ ws; lock; name }
$state = [hashtable]::Synchronized(@{ next = 1 })

function Write-Log($text, $color = 'Gray') { Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $text) -ForegroundColor $color }

# Listen on every name (friends arrive through Tailscale Funnel with its https name as the Host).
# Windows wants a one-time OK for that ("urlacl"); if it is missing we ask for it once (one Yes popup),
# and if that is refused we fall back to this PC only.
function Open-Listener([string]$prefix) {
    $l = [System.Net.HttpListener]::new()
    $l.Prefixes.Add($prefix)
    $l.Start()
    return $l
}
$listener = $null
try { $listener = Open-Listener "http://+:$Port/" } catch {
    if ($IsLinux -or $IsMacOS) { $listener = $null } else {
        Write-Log "First run: Windows needs a one-time OK so friends can reach the server (click Yes)." 'Yellow'
        try {
            Start-Process netsh -ArgumentList "http add urlacl url=http://+:$Port/ user=`"$env:USERDOMAIN\$env:USERNAME`"" -Verb RunAs -Wait -WindowStyle Hidden
            $listener = Open-Listener "http://+:$Port/"
        } catch { $listener = $null }
    }
}
if (-not $listener) {
    try {
        $listener = [System.Net.HttpListener]::new()
        $listener.Prefixes.Add("http://localhost:$Port/")
        $listener.Prefixes.Add("http://127.0.0.1:$Port/")
        $listener.Start()
        Write-Log "Running for this PC only (friends cannot join until Windows allows the port)." 'Yellow'
    } catch {
        Write-Log "Could not open port $Port. Is the server already running in another window?" 'Red'
        Read-Host 'Press Enter to close'
        exit 1
    }
}

# one receive loop per player, each in its own runspace
$pool = [runspacefactory]::CreateRunspacePool(1, 64)
$pool.Open()
$receiver = {
    param($id, $clients)
    $me = $clients[$id]
    $ws = $me.ws
    $buf = New-Object byte[] 65536
    $none = [System.Threading.CancellationToken]::None
    function Send-All([byte[]]$bytes, $skip) {
        foreach ($k in @($clients.Keys)) {
            if ($k -eq $skip) { continue }
            $c = $clients[$k]
            if (-not $c) { continue }
            [System.Threading.Monitor]::Enter($c.lock)
            try {
                if ($c.ws.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
                    $c.ws.SendAsync([ArraySegment[byte]]::new($bytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $none).GetAwaiter().GetResult()
                }
            } catch {} finally { [System.Threading.Monitor]::Exit($c.lock) }
        }
    }
    try {
        while ($ws.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
            $ms = [System.IO.MemoryStream]::new()
            do {
                $r = $ws.ReceiveAsync([ArraySegment[byte]]::new($buf), $none).GetAwaiter().GetResult()
                if ($r.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { throw 'closed' }
                $ms.Write($buf, 0, $r.Count)
            } while (-not $r.EndOfMessage)
            $text = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
            if ($text.Length -lt 2 -or $text[0] -ne '{' -or $text.Length -gt 4000) { continue }
            # stamp the sender's id on the message and pass it to everyone else
            Send-All ([System.Text.Encoding]::UTF8.GetBytes('{"id":' + $id + ',' + $text.Substring(1))) $id
        }
    } catch {} finally {
        $clients.Remove($id)
        Send-All ([System.Text.Encoding]::UTF8.GetBytes('{"id":' + $id + ',"t":"bye"}')) $id
        try { $ws.Dispose() } catch {}
    }
}

Write-Log "tfatftd.xyz server running on port $Port. Leave this window open while you play. Ctrl+C stops it." 'Green'
$jobs = New-Object System.Collections.ArrayList
while ($listener.IsListening) {
    $ctx = $listener.GetContext()
    try {
        if ($ctx.Request.IsWebSocketRequest) {
            $wsctx = $ctx.AcceptWebSocketAsync([NullString]::Value).GetAwaiter().GetResult()
            $id = $state.next; $state.next = $id + 1
            $clients[$id] = @{ ws = $wsctx.WebSocket; lock = [object]::new() }
            $ps = [powershell]::Create()
            $ps.RunspacePool = $pool
            [void]$ps.AddScript($receiver).AddArgument($id).AddArgument($clients)
            [void]$jobs.Add(@{ ps = $ps; handle = $ps.BeginInvoke() })
            Write-Log ("player {0} joined from {1} ({2} online)" -f $id, $ctx.Request.RemoteEndPoint.Address, $clients.Count) 'Cyan'
        } else {
            $body = [System.Text.Encoding]::UTF8.GetBytes("tfatftd.xyz server v0.01 - $($clients.Count) player(s) online")
            $ctx.Response.Headers.Add('Access-Control-Allow-Origin', '*')
            $ctx.Response.ContentType = 'text/plain'
            $ctx.Response.OutputStream.Write($body, 0, $body.Length)
            $ctx.Response.Close()
        }
    } catch { Write-Log "connection error: $($_.Exception.Message)" 'DarkYellow' }
    # tidy finished receive loops
    foreach ($j in @($jobs)) { if ($j.handle.IsCompleted) { try { $j.ps.EndInvoke($j.handle) } catch {}; $j.ps.Dispose(); $jobs.Remove($j) } }
}
