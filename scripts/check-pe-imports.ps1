# Import guard for the RentaPC setup window (T112, correction 6).
#
# Reads the normal and the delay import tables of a PE32 or PE32+ file and rejects the files that import
# the Visual C++ runtime DLLs (msvcp140.dll, vcruntime140.dll, vcruntime140_1.dll): a PC without the
# Visual C++ Redistributable cannot start such a program. The headers are parsed here, with no dumpbin and
# no substring scan: DOS header, NT signature, optional header (PE32 or PE32+), data directories 1 (imports)
# and 13 (delay imports), RVA to file offset through the section table.
#
# Usage:
#     pwsh -File scripts\check-pe-imports.ps1 -Path RentaPC-Setup.exe
#
# One line goes to the output stream and the exit code follows it:
#     PE-IMPORTS: ok sha256=HEX imports=NAMES          exit 0   none of the three DLLs is imported
#     PE-IMPORTS: forbidden sha256=HEX found=NAMES     exit 1   at least one is imported
#     PE-IMPORTS: error REASON                         exit 2   missing, unreadable or malformed file
# HEX is the file's lowercase SHA-256; NAMES are the lowercase DLL names, sorted, joined by commas.
# DLL names are compared case-insensitively. Nothing is written to the error stream.

[CmdletBinding()]
param(
    [string]$Path = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Forbidden = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$MaxDescriptors = 4096
$MaxNameLength = 260

function Fail([string]$reason) { throw [System.IO.InvalidDataException]::new($reason) }

function Need([byte[]]$b, [long]$offset, [long]$count, [string]$what) {
    if ($offset -lt 0 -or $count -lt 0 -or ($offset + $count) -gt $b.LongLength) { Fail "truncated file: $what is outside the file" }
}
function U16([byte[]]$b, [long]$o, [string]$what) { Need $b $o 2 $what; return [uint32][BitConverter]::ToUInt16($b, [int]$o) }
function U32([byte[]]$b, [long]$o, [string]$what) { Need $b $o 4 $what; return [uint32][BitConverter]::ToUInt32($b, [int]$o) }
function U64([byte[]]$b, [long]$o, [string]$what) { Need $b $o 8 $what; return [uint64][BitConverter]::ToUInt64($b, [int]$o) }

# Section table rows, in file order: VirtualAddress, extent (virtual size, or raw size when that is 0), raw pointer.
function Convert-RvaToOffset([byte[]]$b, [object[]]$sections, [uint64]$rva, [string]$what) {
    foreach ($s in $sections) {
        if ($rva -ge $s.Va -and $rva -lt ($s.Va + $s.Extent)) {
            $offset = [long]($rva - $s.Va + $s.Raw)
            if ($offset -ge $b.LongLength) { Fail "$what points past the end of the file" }
            return $offset
        }
    }
    Fail "$what (RVA 0x$($rva.ToString('x'))) is in no section"
}

function Read-DllName([byte[]]$b, [long]$offset, [string]$what) {
    $chars = [System.Text.StringBuilder]::new()
    for ($i = 0; $i -lt $MaxNameLength; $i++) {
        Need $b ($offset + $i) 1 $what
        $c = $b[$offset + $i]
        if ($c -eq 0) {
            if ($chars.Length -eq 0) { Fail "$what is empty" }
            return $chars.ToString().ToLowerInvariant()
        }
        [void]$chars.Append([char]$c)
    }
    Fail "$what is not terminated"
}

function Get-ImportedDlls([byte[]]$b) {
    if ($b.LongLength -lt 64) { Fail 'file is too small to be a PE file' }
    if ($b[0] -ne 0x4D -or $b[1] -ne 0x5A) { Fail 'not a PE file (no MZ header)' }
    $pe = [long](U32 $b 0x3C 'e_lfanew')
    Need $b $pe 24 'NT headers'
    if ($b[$pe] -ne 0x50 -or $b[$pe + 1] -ne 0x45 -or $b[$pe + 2] -ne 0 -or $b[$pe + 3] -ne 0) { Fail 'not a PE file (no PE signature)' }

    $sectionCount = [long](U16 $b ($pe + 6) 'NumberOfSections')
    $optSize = [long](U16 $b ($pe + 20) 'SizeOfOptionalHeader')
    $opt = $pe + 24
    Need $b $opt $optSize 'optional header'
    $magic = U16 $b $opt 'optional header magic'
    if ($magic -eq 0x10B) { $is64 = $false; $dirsAt = 96; $countAt = 92 }
    elseif ($magic -eq 0x20B) { $is64 = $true; $dirsAt = 112; $countAt = 108 }
    else { Fail ('unknown optional header magic 0x' + $magic.ToString('x')) }
    if ($optSize -lt ($countAt + 4)) { Fail 'optional header is too small' }

    $imageBase = if ($is64) { U64 $b ($opt + 24) 'ImageBase' } else { [uint64](U32 $b ($opt + 28) 'ImageBase') }
    $dirCount = [long](U32 $b ($opt + $countAt) 'NumberOfRvaAndSizes')
    $fit = [long][Math]::Floor(($optSize - $dirsAt) / 8)
    if ($dirCount -gt $fit) { $dirCount = $fit }

    $sectionsAt = $opt + $optSize
    Need $b $sectionsAt ($sectionCount * 40) 'section table'
    $sections = @()
    for ($i = 0; $i -lt $sectionCount; $i++) {
        $at = $sectionsAt + 40 * $i
        $virtualSize = [uint64](U32 $b ($at + 8) 'section VirtualSize')
        $rawSize = [uint64](U32 $b ($at + 16) 'section SizeOfRawData')
        $sections += [pscustomobject]@{
            Va     = [uint64](U32 $b ($at + 12) 'section VirtualAddress')
            Extent = if ($virtualSize -gt 0) { $virtualSize } else { $rawSize }
            Raw    = [uint64](U32 $b ($at + 20) 'section PointerToRawData')
        }
    }

    $names = [System.Collections.Generic.List[string]]::new()

    # Data directory 1: IMAGE_IMPORT_DESCRIPTOR, 20 bytes each, Name is the RVA at +12, ends with an all-zero row.
    if ($dirCount -gt 1) {
        $rva = [uint64](U32 $b ($opt + $dirsAt + 8) 'import directory RVA')
        if ($rva -ne 0) {
            $at = Convert-RvaToOffset $b $sections $rva 'the import table'
            for ($n = 0; ; $n++) {
                if ($n -ge $MaxDescriptors) { Fail 'import table is not terminated' }
                $row = $at + 20 * $n
                Need $b $row 20 'an import descriptor'
                $nameRva = [uint64](U32 $b ($row + 12) 'import descriptor Name')
                $allZero = $true
                foreach ($k in 0..19) { if ($b[$row + $k] -ne 0) { $allZero = $false; break } }
                if ($allZero -or $nameRva -eq 0) { break }
                $nameAt = Convert-RvaToOffset $b $sections $nameRva 'an import DLL name'
                $names.Add((Read-DllName $b $nameAt 'an import DLL name'))
            }
        }
    }

    # Data directory 13: IMAGE_DELAYLOAD_DESCRIPTOR, 32 bytes each. Attributes bit 0 set means the fields are RVAs;
    # clear means they are virtual addresses (ImageBase included). Name is at +4.
    if ($dirCount -gt 13) {
        $rva = [uint64](U32 $b ($opt + $dirsAt + 13 * 8) 'delay import directory RVA')
        if ($rva -ne 0) {
            $at = Convert-RvaToOffset $b $sections $rva 'the delay import table'
            for ($n = 0; ; $n++) {
                if ($n -ge $MaxDescriptors) { Fail 'delay import table is not terminated' }
                $row = $at + 32 * $n
                Need $b $row 32 'a delay import descriptor'
                $attributes = U32 $b $row 'delay import Attributes'
                $nameField = [uint64](U32 $b ($row + 4) 'delay import Name')
                if ($nameField -eq 0) { break }
                if (($attributes -band 1) -eq 1) { $nameRva = $nameField }
                else {
                    if ($nameField -lt $imageBase) { Fail 'a delay import DLL name address is below the image base' }
                    $nameRva = $nameField - $imageBase
                }
                $nameAt = Convert-RvaToOffset $b $sections $nameRva 'a delay import DLL name'
                $names.Add((Read-DllName $b $nameAt 'a delay import DLL name'))
            }
        }
    }

    $unique = [System.Collections.Generic.List[string]]::new([System.Collections.Generic.HashSet[string]]::new($names, [StringComparer]::Ordinal))
    $unique.Sort([StringComparer]::Ordinal)
    return , $unique.ToArray()
}

$line = ''
$code = 2
try {
    if (-not $Path) { Fail 'no -Path given' }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Fail "file not found: $Path" }
    try { $bytes = [System.IO.File]::ReadAllBytes($Path) }
    catch { Fail "cannot read $Path ($($_.Exception.Message))" }

    $imports = Get-ImportedDlls $bytes
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $hex = ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }

    $found = @($imports | Where-Object { $Forbidden -contains $_ })
    if ($found.Count -gt 0) {
        $line = 'PE-IMPORTS: forbidden sha256={0} found={1}' -f $hex, ($found -join ',')
        $code = 1
    }
    else {
        $line = 'PE-IMPORTS: ok sha256={0} imports={1}' -f $hex, ($imports -join ',')
        $code = 0
    }
}
catch {
    $reason = ($_.Exception.Message -replace '\s+', ' ').Trim()
    $line = "PE-IMPORTS: error $reason"
    $code = 2
}

Write-Output $line
exit $code
