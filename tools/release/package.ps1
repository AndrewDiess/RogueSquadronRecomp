# Packages a Windows release from a Release build: the program, its DLLs and assets, the player README, this
# project's license and the licenses of everything bundled. Writes <OutDir>\RogueSquadronRecompiled-v<Version>-windows
# and a .zip of it.
# Usage: powershell -ExecutionPolicy Bypass -File tools\release\package.ps1 -BuildDir <release build dir> -Version 1.0.0 -OutDir <dir>
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$OutDir
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$name = "RogueSquadronRecompiled-v$Version-windows"
$stage = Join-Path $OutDir $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage | Out-Null

# Program files. The assets come from the repository (the build folder can hold stale copies).
foreach ($f in "RogueSquadronRecompiled.exe", "SDL2.dll", "dxil.dll", "dxcompiler.dll") {
    Copy-Item (Join-Path $BuildDir $f) $stage
}
Copy-Item (Join-Path $repo "recompcontrollerdb.txt") $stage
Copy-Item (Join-Path $repo "assets") (Join-Path $stage "assets") -Recurse
Copy-Item (Join-Path $PSScriptRoot "README.txt") $stage
Copy-Item (Join-Path $repo "COPYING") (Join-Path $stage "LICENSE.txt")

# Third-party licenses: <name>\<file> for each bundled component.
$licenses = Join-Path $stage "licenses"
$sources = [ordered]@{
    "RT64"                       = "lib\rt64\LICENSE"
    "N64ModernRuntime"           = "lib\N64ModernRuntime\COPYING"
    "N64Recomp"                  = "lib\N64ModernRuntime\N64Recomp\LICENSE"
    "miniz"                      = "lib\N64ModernRuntime\thirdparty\miniz\LICENSE"
    "o1heap"                     = "lib\N64ModernRuntime\thirdparty\o1heap\LICENSE"
    "xxHash"                     = "lib\N64ModernRuntime\thirdparty\xxHash\LICENSE"
    "RmlUi"                      = "lib\RmlUi\LICENSE.txt"
    "lunasvg"                    = "lib\lunasvg\LICENSE"
    "FreeType"                   = "lib\freetype-windows-binaries\LICENSE.TXT"
    "GamepadMotionHelpers"       = "lib\GamepadMotionHelpers\LICENSE"
    "SlotMap"                    = "lib\SlotMap\README.md"
    "D3D12MemoryAllocator"       = "lib\rt64\src\contrib\D3D12MemoryAllocator\LICENSE.txt"
    "VulkanMemoryAllocator"      = "lib\rt64\src\contrib\VulkanMemoryAllocator\LICENSE.txt"
    "Vulkan-Headers"             = "lib\rt64\src\contrib\Vulkan-Headers\LICENSE.md"
    "volk"                       = "lib\rt64\src\contrib\volk\LICENSE.md"
    "plume"                      = "lib\rt64\src\contrib\plume\LICENSE"
    "hlslpp"                     = "lib\rt64\src\contrib\hlslpp\LICENSE"
    "ddspp"                      = "lib\rt64\src\contrib\ddspp\LICENSE"
    "imgui"                      = "lib\rt64\src\contrib\imgui\LICENSE.txt"
    "implot"                     = "lib\rt64\src\contrib\implot\LICENSE"
    "im3d"                       = "lib\rt64\src\contrib\im3d\LICENSE"
    "nativefiledialog-extended"  = "lib\rt64\src\contrib\nativefiledialog-extended\LICENSE"
    "re-spirv"                   = "lib\rt64\src\contrib\re-spirv\LICENSE"
    "SPIRV-Cross"                = "lib\rt64\src\contrib\spirv-cross\LICENSE"
    "stb"                        = "lib\rt64\src\contrib\stb\LICENSE"
    "xxHash (RT64)"              = "lib\rt64\src\contrib\xxHash\LICENSE"
    "zstd"                       = "lib\rt64\src\contrib\zstd\LICENSE"
    "Lato font"                  = "assets\LICENSE-Lato.txt"
    "Noto Emoji font"            = "assets\LICENSE-NotoEmoji.txt"
    "PromptFont"                 = "assets\promptfont\LICENSE.txt"
}
foreach ($entry in $sources.GetEnumerator()) {
    $dir = Join-Path $licenses $entry.Key
    New-Item -ItemType Directory $dir -Force | Out-Null
    Copy-Item (Join-Path $repo $entry.Value) $dir
}

# Licenses that live outside the repository: SDL2 (fetched by the build) and the DirectX Shader Compiler DLLs
# (from Microsoft's release package; dxil.dll is the signed Microsoft binary, see LICENSE-MS.txt).
New-Item -ItemType Directory (Join-Path $licenses "SDL2") -Force | Out-Null
Copy-Item (Join-Path $BuildDir "_deps\sdl2-src\COPYING.txt") (Join-Path $licenses "SDL2")
New-Item -ItemType Directory (Join-Path $licenses "DirectXShaderCompiler") -Force | Out-Null
Copy-Item (Join-Path $PSScriptRoot "dxc\*") (Join-Path $licenses "DirectXShaderCompiler")

# moodycamel::ConcurrentQueue keeps its license at the top of its header.
New-Item -ItemType Directory (Join-Path $licenses "concurrentqueue") -Force | Out-Null
(Get-Content (Join-Path $repo "lib\concurrentqueue\concurrentqueue.h") -TotalCount 30 | Where-Object { $_ -like "//*" }) -replace '^// ?', '' |
    Set-Content (Join-Path $licenses "concurrentqueue\LICENSE.txt")

$zip = "$stage.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
"{0}: {1:N1} MB unpacked, {2:N1} MB zipped" -f $name, ((Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), ((Get-Item $zip).Length / 1MB)
