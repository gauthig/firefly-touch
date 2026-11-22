<#
  Capture the bus-survey console (docs/BUS-SURVEY.md) and let a second process send
  it single-character commands.

    pwsh tools\survey_capture.ps1 -Port COM7 -Log survey.log

  Send a command from another shell by writing it to the command file:

    Set-Content survey.cmd 'x'     # stop the probe; also: t a b d c p 1 2 h

  DTR and RTS are held LOW: on this board family they drive EN/BOOT, and opening a
  port with them asserted resets the chip (sometimes into ROM download mode).
  Works on either port: the survey build reads commands from the UART console and
  from the USB-Serial/JTAG RX FIFO. Use -ResetOnStart to log from the boot banner.
#>
param(
    [Parameter(Mandatory)][string]$Port,
    [int]$Baud = 115200,
    [string]$Log = "survey.log",
    [string]$CmdFile = "survey.cmd",
    [switch]$ResetOnStart   # one clean EN pulse (DTR held low) so the log starts at boot
)

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout = 200
$sp.NewLine = "`n"
$sp.Open()
$w = [System.IO.StreamWriter]::new($Log, $true)
$w.AutoFlush = $true

# Build the whole line first: inside a method call PowerShell would split "-f a, b" into
# separate method arguments instead of one format operand list.
function Stamp([string]$text) { return ('{0} {1}' -f (Get-Date -Format 'HH:mm:ss.fff'), $text) }

$w.WriteLine((Stamp "--- capture started on $Port @ $Baud ---"))

if ($ResetOnStart) {
    $sp.DtrEnable = $false          # IO0 stays high: boot the app, never ROM download
    $sp.RtsEnable = $true           # EN low
    Start-Sleep -Milliseconds 100
    $sp.RtsEnable = $false          # EN high: chip boots
    $w.WriteLine((Stamp "--- reset pulse sent ---"))
}

try {
    while ($true) {
        try {
            $line = $sp.ReadLine()
            $clean = $line -replace '\x1b\[[0-9;]*m', ''
            $w.WriteLine((Stamp $clean.TrimEnd()))
        } catch [System.TimeoutException] { }

        if (Test-Path $CmdFile) {
            $c = (Get-Content $CmdFile -Raw).Trim()
            Remove-Item $CmdFile -ErrorAction SilentlyContinue
            foreach ($ch in $c.ToCharArray()) {
                $sp.Write([string]$ch)
                Start-Sleep -Milliseconds 30
            }
            $w.WriteLine((Stamp ">>> sent '$c'"))
        }
    }
}
finally {
    $sp.Close()
    $w.Close()
}
