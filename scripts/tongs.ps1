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
    [switch]$StopOnError
)

# Client for \\.\pipe\tongs: sends each command line and prints its one-line JSON answer.
# Long commands (run_until, wait_until) answer only when they finish.
$ErrorActionPreference = 'Stop'
$pipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', 'tongs', [System.IO.Pipes.PipeDirection]::InOut)
$failed = $false
try {
    $pipe.Connect($TimeoutMs)
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    $writer = [System.IO.StreamWriter]::new($pipe, $utf8, 4096, $true)
    $writer.NewLine = "`n"
    $reader = [System.IO.StreamReader]::new($pipe, $utf8, $false, 4096, $true)
    foreach ($line in $Command) {
        $writer.WriteLine($line)
        $writer.Flush()
        $answer = $reader.ReadLine()
        if ($null -eq $answer) { throw "The pipe closed without an answer to: $line" }
        if ($AsObject) { $answer | ConvertFrom-Json } else { $answer }
        if ($StopOnError -and ($answer | ConvertFrom-Json).ok -ne $true) {
            $failed = $true
            break
        }
    }
}
finally {
    $pipe.Dispose()
}
if ($failed) { exit 1 }
