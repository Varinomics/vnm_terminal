$ErrorActionPreference = 'Stop'
if ($args.Count -ne 7) {
    throw "Expected report path, exit code and five arguments, got $($args.Count) values."
}
[IO.File]::WriteAllText($args[0], (ConvertTo-Json -InputObject @($args[2..6]) -Compress))
[Console]::WriteLine('VNM_PACKAGE_ARGUMENTS_PRESERVED')
exit [int]$args[1]
