[CmdletBinding()]
param(
    # One protocol command, e.g. ping, state, help.
    [Parameter(Position = 0)][string]$Command = 'state',
    [ValidateRange(100, 60000)][int]$TimeoutMs = 2000,
    # Return the parsed object instead of the raw JSON line.
    [switch]$AsObject
)

# Client for \\.\pipe\tongs: sends one command line, prints the one-line JSON answer.
$ErrorActionPreference = 'Stop'
$pipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', 'tongs', [System.IO.Pipes.PipeDirection]::InOut)
try {
    $pipe.Connect($TimeoutMs)
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    $writer = [System.IO.StreamWriter]::new($pipe, $utf8, 4096, $true)
    $writer.NewLine = "`n"
    $writer.WriteLine($Command)
    $writer.Flush()
    $reader = [System.IO.StreamReader]::new($pipe, $utf8, $false, 4096, $true)
    $line = $reader.ReadLine()
    if ($null -eq $line) { throw 'The pipe closed without an answer.' }
    if ($AsObject) { $line | ConvertFrom-Json } else { $line }
}
finally {
    $pipe.Dispose()
}
