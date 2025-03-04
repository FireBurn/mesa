
$MyPath = $MyInvocation.MyCommand.Path | Split-Path -Parent
. "$MyPath\mesa_init_msvc.ps1"

# we want more secure TLS 1.2 for most things, but it breaks SourceForge
# downloads so must be done after Chocolatey use
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13;

Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "deps" | Out-Null

$depsInstallPath="C:\mesa-deps"

Get-Date
Write-Host "Cloning DirectX-Headers"
git clone -b v1.614.1 --depth=1 https://github.com/microsoft/DirectX-Headers deps/DirectX-Headers
if (!$?) {
  Write-Host "Failed to clone DirectX-Headers repository"
  Exit 1
}
Write-Host "Building DirectX-Headers"
$dxheaders_build = New-Item -ItemType Directory -Path ".\deps\DirectX-Headers" -Name "build"
Push-Location -Path $dxheaders_build.FullName
meson setup .. --backend=ninja -Dprefix="$depsInstallPath" --buildtype=release -Db_vscrt=mt && `
ninja -j32 install
$buildstatus = $?
Pop-Location
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path $dxheaders_build
if (!$buildstatus) {
  Write-Host "Failed to compile DirectX-Headers"
  Exit 1
}

Get-Date
Write-Host "Cloning zlib"
git clone -b v1.3.1 --depth=1 https://github.com/madler/zlib deps/zlib
if (!$?) {
  Write-Host "Failed to clone zlib repository"
  Exit 1
}
Write-Host "Downloading zlib meson build files"
Invoke-WebRequest -Uri "https://wrapdb.mesonbuild.com/v2/zlib_1.3.1-1/get_patch" -OutFile deps/zlib.zip
Expand-Archive -Path deps/zlib.zip -Destination deps/zlib
# Wrap archive puts build files in a version subdir
robocopy deps/zlib/zlib-1.3.1 deps/zlib /E
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path deps/zlib/zlib-1.3.1
$zlib_build = New-Item -ItemType Directory -Path ".\deps\zlib" -Name "build"
Push-Location -Path $zlib_build.FullName
meson setup .. --backend=ninja -Dprefix="$depsInstallPath" --default-library=static --buildtype=release -Db_vscrt=mt && `
ninja -j32 install
$buildstatus = $?
Pop-Location
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path $zlib_build
if (!$buildstatus) {
  Write-Host "Failed to compile zlib"
  Exit 1
}

Get-Date
Write-Host "Cloning LLVM release/15.x"
git clone -b llvmorg-19.1.6 --depth=1 https://github.com/llvm/llvm-project deps/llvm-project
if (!$?) {
  Write-Host "Failed to clone LLVM repository"
  Exit 1
}

Get-Date
Write-Host "Cloning SPIRV-LLVM-Translator"
git clone -b v19.1.3 https://github.com/KhronosGroup/SPIRV-LLVM-Translator deps/llvm-project/llvm/projects/SPIRV-LLVM-Translator
if (!$?) {
  Write-Host "Failed to clone SPIRV-LLVM-Translator repository"
  Exit 1
}

Get-Date
# slightly convoluted syntax but avoids the CWD being under the PS filesystem meta-path
$llvm_build = New-Item -ItemType Directory -ErrorAction SilentlyContinue -Force -Path ".\deps\llvm-project" -Name "build"
Push-Location -Path $llvm_build.FullName
Write-Host "Compiling LLVM and Clang"
cmake ../llvm `
-GNinja `
-DCMAKE_BUILD_TYPE=Release `
-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
-DCMAKE_PREFIX_PATH="$depsInstallPath" `
-DCMAKE_INSTALL_PREFIX="$depsInstallPath" `
-DLLVM_ENABLE_PROJECTS="clang" `
-DLLVM_TARGETS_TO_BUILD="AMDGPU;X86" `
-DLLVM_OPTIMIZED_TABLEGEN=TRUE `
-DLLVM_ENABLE_ASSERTIONS=TRUE `
-DLLVM_INCLUDE_UTILS=OFF `
-DLLVM_INCLUDE_RUNTIMES=OFF `
-DLLVM_INCLUDE_TESTS=OFF `
-DLLVM_INCLUDE_EXAMPLES=OFF `
-DLLVM_INCLUDE_GO_TESTS=OFF `
-DLLVM_INCLUDE_BENCHMARKS=OFF `
-DLLVM_BUILD_LLVM_C_DYLIB=OFF `
-DLLVM_ENABLE_DIA_SDK=OFF `
-DCLANG_BUILD_TOOLS=ON `
-DLLVM_SPIRV_INCLUDE_TESTS=OFF `
-DLLVM_ENABLE_ZLIB=OFF `
-Wno-dev && `
ninja -j32 install
$buildstatus = $?
Pop-Location
if (!$buildstatus) {
  Write-Host "Failed to compile LLVM"
  Exit 1
}

Get-Date
$libclc_build = New-Item -ItemType Directory -Path ".\deps\llvm-project" -Name "build-libclc"
Push-Location -Path $libclc_build.FullName
Write-Host "Compiling libclc"
# libclc can only be built with Ninja, because CMake's VS backend doesn't know how to compile new language types
cmake ../libclc `
-GNinja `
-DCMAKE_BUILD_TYPE=Release `
-DCMAKE_CXX_FLAGS="-m64" `
-DCMAKE_POLICY_DEFAULT_CMP0091=NEW `
-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
-DCMAKE_INSTALL_PREFIX="$depsInstallPath" `
-DLIBCLC_TARGETS_TO_BUILD="spirv-mesa3d-;spirv64-mesa3d-" && `
ninja -j32 install
$buildstatus = $?
Pop-Location
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path $libclc_build
if (!$buildstatus) {
  Write-Host "Failed to compile libclc"
  Exit 1
}
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path $llvm_build

Get-Date
Write-Host "Cloning SPIRV-Tools"
git clone -b "vulkan-sdk-$env:VULKAN_SDK_VERSION" --depth=1 https://github.com/KhronosGroup/SPIRV-Tools deps/SPIRV-Tools
if (!$?) {
  Write-Host "Failed to clone SPIRV-Tools repository"
  Exit 1
}
git clone -b "vulkan-sdk-$env:VULKAN_SDK_VERSION" --depth=1 https://github.com/KhronosGroup/SPIRV-Headers deps/SPIRV-Tools/external/SPIRV-Headers
if (!$?) {
  Write-Host "Failed to clone SPIRV-Headers repository"
  Exit 1
}
Write-Host "Building SPIRV-Tools"
$spv_build = New-Item -ItemType Directory -Path ".\deps\SPIRV-Tools" -Name "build"
Push-Location -Path $spv_build.FullName
# SPIRV-Tools doesn't use multi-threaded MSVCRT, but we need it to
cmake .. `
-GNinja `
-DCMAKE_BUILD_TYPE=Release `
-DCMAKE_POLICY_DEFAULT_CMP0091=NEW `
-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
-DCMAKE_INSTALL_PREFIX="$depsInstallPath" && `
ninja -j32 install
$buildstatus = $?
Pop-Location
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path $spv_build
if (!$buildstatus) {
  Write-Host "Failed to compile SPIRV-Tools"
  Exit 1
}

function Remove-Symlinks {
  Get-ChildItem -Force -ErrorAction SilentlyContinue @Args | Where-Object { if($_.Attributes -match "ReparsePoint"){$_.Delete()} }
}
Remove-Symlinks -Path "deps" -Recurse
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path "deps" | Out-Null

Get-Date
Write-Host "Complete"
