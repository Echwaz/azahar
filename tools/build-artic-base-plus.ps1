param([string]$BuildDirectory = "build-artic-base-plus", [int]$Jobs = 3)
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path $PSScriptRoot -Parent
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
# Run from an x64 Visual Studio developer shell with CMake, Ninja, Git and Rust on PATH.
Push-Location $repoRoot
try {
    Invoke-Checked git @("submodule", "update", "--init", "--recursive")
    Invoke-Checked cmake @("-S", ".", "-B", $BuildDirectory, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        "-DENABLE_QT=ON", "-DENABLE_SDL2=ON", "-DENABLE_SDL2_FRONTEND=OFF",
        "-DENABLE_TESTS=ON", "-DENABLE_FFMPEG=OFF", "-DENABLE_OPENAL=OFF",
        "-DENABLE_SCRIPTING=OFF", "-DENABLE_WEB_SERVICE=OFF", "-DENABLE_LTO=OFF",
        "-DENABLE_QT_UPDATE_CHECKER=OFF", "-DENABLE_QT_TRANSLATION=OFF")
    Invoke-Checked cmake @("--build", $BuildDirectory, "--target", "citra_qt", "tests", "--parallel", "$Jobs")
    Invoke-Checked cargo @("build", "--release", "--locked", "--target", "x86_64-pc-windows-msvc",
        "--manifest-path", "tools/home_layout/Cargo.toml")
    $executable = Get-ChildItem $BuildDirectory -Recurse -Filter "azahar.exe" |
        Where-Object { $_.DirectoryName -match '[\\/]bin([\\/]|$)' } | Select-Object -First 1
    if (!$executable) { throw "azahar.exe was not found in the build output" }
    Copy-Item "tools/home_layout/target/x86_64-pc-windows-msvc/release/artic_home_layout.dll" $executable.DirectoryName -Force
    Invoke-Checked ctest @("--test-dir", $BuildDirectory, "-C", "Release", "--output-on-failure")
    Write-Host "Artic Base++ executable: $($executable.FullName)"
} finally { Pop-Location }
