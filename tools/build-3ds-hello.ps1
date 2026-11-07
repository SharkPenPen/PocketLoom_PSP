param([string]$BuildDir = "build/3ds-hello")
$ErrorActionPreference = "Stop"
if (-not $env:DEVKITPRO) { throw "DEVKITPRO is not set" }
cmake -S tools/3ds-hello -B $BuildDir -G Ninja `
  "-DCMAKE_TOOLCHAIN_FILE=tools/3ds-hello/cmake/toolchain-3ds.cmake" `
  "-DDEVKITPRO=$env:DEVKITPRO"
cmake --build $BuildDir
