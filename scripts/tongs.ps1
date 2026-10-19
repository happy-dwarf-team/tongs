[CmdletBinding()]
param(
    # Protocol commands, sent in order over one connection, e.g. state, pause, 'speed 5',
    # 'run_until 1720.1.1 speed=5'. One answer line is printed per command.
    [Parameter(Position = 0, ValueFromRemainingArguments)][string[]]$Command = @('state'),
    # How long to wait for the pipe to accept the connection.
    [ValidateRange(100, 60000)][int]$TimeoutMs = 2000,
    # Return parsed objects instead of the raw JSON lines.
    [switch]$AsObject,
    # Stop at the first answer with "ok":false and exit with code 1.
    [switch]$StopOnError,
    # After the commands: subscribe to these kinds (e.g. 'state,date,event date=day' or 'all')
    # and print notification lines until -Seconds or -Count is reached, then unsubscribe.
    [string]$Subscribe,
    [ValidateRange(1, 86400)][int]$Seconds = 60,
    [ValidateRange(0, 10000000)][int]$Count = 0
)

# Client for \\.\pipe\tongs: sends each command line and prints its one-line JSON answer.
# Long commands (run_until, wait_until) answer only when they finish.
$ErrorActionPreference = 'Stop'
# Asynchronous (overlapped): `unsubscribe` is written while a ReadLineAsync is pending, and
# synchronous I/O on one pipe handle would make the write wait for that read.
$pipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', 'tongs', [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::Asynchronous)
$failed = $false
try {
    $pipe.Connect($TimeoutMs)
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    $writer = [System.IO.StreamWriter]::new($pipe, $utf8, 4096, $true)
    $writer.NewLine = "`n"
    $reader = [System.IO.StreamReader]::new($pipe, $utf8, $false, 4096, $true)
    $emit = { param($line) if ($AsObject) { $line | ConvertFrom-Json } else { $line } }
    foreach ($line in $Command) {
        $writer.WriteLine($line)
        $writer.Flush()
        $answer = $reader.ReadLine()
        if ($null -eq $answer) { throw "The pipe closed without an answer to: $line" }
        & $emit $answer
        if ($StopOnError -and ($answer | ConvertFrom-Json).ok -ne $true) {
            $failed = $true
            break
        }
    }
    if ($Subscribe -and -not $failed) {
        $writer.WriteLine("subscribe $Subscribe")
        $writer.Flush()
        $answer = $reader.ReadLine()
        if ($null -eq $answer) { throw 'The pipe closed without an answer to subscribe' }
        & $emit $answer
        if (($answer | ConvertFrom-Json).ok -ne $true) {
            $failed = $true
        } else {
            # One ReadLineAsync at a time; a line that arrives after the deadline is left unread.
            $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
            $received = 0
            $pending = $reader.ReadLineAsync()
            while (($Count -eq 0 -or $received -lt $Count) -and [DateTime]::UtcNow -lt $deadline) {
                $left = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
                if (-not $pending.Wait($left)) { break }
                if ($null -eq $pending.Result) { throw 'The pipe closed during the subscription' }
                & $emit $pending.Result
                $received++
                $pending = $reader.ReadLineAsync()
            }
            $writer.WriteLine('unsubscribe')
            $writer.Flush()
            # Notifications already on their way come before the answer.
            for (;;) {
                $pending.Wait() | Out-Null
                $line = $pending.Result
                if ($null -eq $line) { throw 'The pipe closed without an answer to unsubscribe' }
                if ($line.StartsWith('{"notification":')) {
                    $pending = $reader.ReadLineAsync()
                    continue
                }
                & $emit $line
                break
            }
        }
    }
}
finally {
    $pipe.Dispose()
}
if ($failed) { exit 1 }
