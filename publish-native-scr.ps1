param(
    [Parameter(Mandatory = $true)]
    [string]$VideoPath,

    [string]$FfmpegPath = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$assetsDir = Join-Path $projectRoot "Assets"
$projectFile = Join-Path $projectRoot "NativeVideoScrSaver.vcxproj"
$publishDir = Join-Path $projectRoot "publish"
$videoItem = Get-Item -LiteralPath $VideoPath
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
$objDir = Join-Path $projectRoot "obj\Release\x64"
$binDir = Join-Path $projectRoot "bin\Release\x64"
$normalizedVideoPath = Join-Path $assetsDir "normalized-input.mp4"

if ([string]::IsNullOrWhiteSpace($FfmpegPath)) {
    $candidates = @(
        "D:\ffmpeg-master-latest-win64-gpl-shared\bin\ffmpeg.exe",
        "ffmpeg"
    )

    foreach ($candidate in $candidates) {
        try {
            if ($candidate -eq "ffmpeg") {
                & $candidate -version *> $null
                if ($LASTEXITCODE -eq 0) {
                    $FfmpegPath = $candidate
                    break
                }
            } elseif (Test-Path -LiteralPath $candidate) {
                $FfmpegPath = $candidate
                break
            }
        } catch {
        }
    }
}

if ([string]::IsNullOrWhiteSpace($FfmpegPath)) {
    throw "ffmpeg was not found. Pass -FfmpegPath or install ffmpeg."
}

New-Item -ItemType Directory -Path $assetsDir -Force | Out-Null

# Normalize every input into a Windows-native friendly MP4 payload so the screen saver
# always embeds a predictable H.264/AAC file instead of relying on arbitrary source codecs.
& $FfmpegPath -y -i $videoItem.FullName `
    -vf "format=yuv420p" `
    -c:v libx264 -preset medium -profile:v high -level 4.1 -pix_fmt yuv420p -crf 18 `
    -movflags +faststart `
    -c:a aac -b:a 192k `
    $normalizedVideoPath

if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $normalizedVideoPath)) {
    throw "ffmpeg failed to normalize input video."
}

Copy-Item -LiteralPath $normalizedVideoPath -Destination (Join-Path $assetsDir "PackagedVideo.bin") -Force

# Force a fresh native resource rebuild so the embedded video always matches the selected file.
if (Test-Path -LiteralPath $objDir) {
    Get-ChildItem -LiteralPath $objDir -Filter "NativeVideoScrSaver.res" -File -ErrorAction SilentlyContinue | Remove-Item -Force
}

if (Test-Path -LiteralPath $binDir) {
    Get-ChildItem -LiteralPath $binDir -Filter "NativeVideoScrSaver.exe" -File -ErrorAction SilentlyContinue | Remove-Item -Force
}

& $msbuild $projectFile /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m

if ($LASTEXITCODE -ne 0) {
    throw "Native build failed with exit code $LASTEXITCODE."
}

$outputDir = Join-Path $projectRoot "bin\Release\x64"
$exePath = Join-Path $outputDir "NativeVideoScrSaver.exe"
$scrName = "{0}.scr" -f $videoItem.BaseName
$scrPath = Join-Path $publishDir $scrName

New-Item -ItemType Directory -Path $publishDir -Force | Out-Null
Copy-Item -LiteralPath $exePath -Destination $scrPath -Force

Write-Host ""
Write-Host "Generated native screen saver:" -ForegroundColor Green
Write-Host $scrPath
Write-Host ""
Write-Host "This build is native, does not depend on .NET, and embeds a normalized MP4 payload for reliable playback." -ForegroundColor Cyan
