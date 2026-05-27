#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build}"
BUILD_TYPE="${1:-release}"

# =============================================================================
# Profili di compilazione predefiniti
# =============================================================================
CMAKE_FLAGS="-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"

case "$BUILD_TYPE" in
    debug|Debug|DEBUG)
        CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Debug"
        ;;
    release|Release|RELEASE)
        CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release"
        ;;
    cuda|CUDA)
        CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release -DLLAMA_CUDA=ON"
        ;;
    vulkan|VULKAN)
        CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release -DLLAMA_VULKAN=ON"
        ;;
    hip|HIP|rocm|ROCM)
        CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release -DLLAMA_HIPBLAS=ON"
        ;;
    metal|METAL)
        CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release -DLLAMA_METAL=ON"
        ;;
    sycl|SYCL)
        CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release -DLLAMA_SYCL=ON"
        ;;
    blas|BLAS)
        CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release -DGGML_BLAS=ON -DGGML_BLAS_VENDOR=${BLAS_VENDOR:-OpenBLAS}"
        ;;
    openmp|OPENMP)
        CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release -DLLAMA_OPENMP=ON"
        ;;
    all|ALL)
        # Abilita tutto ciò che è disponibile (rilevamento automatico)
        CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release"
        CMAKE_FLAGS="$CMAKE_FLAGS -DLLAMA_CUDA=ON"
        CMAKE_FLAGS="$CMAKE_FLAGS -DLLAMA_VULKAN=ON"
        CMAKE_FLAGS="$CMAKE_FLAGS -DLLAMA_HIPBLAS=ON"
        CMAKE_FLAGS="$CMAKE_FLAGS -DLLAMA_METAL=ON"
        CMAKE_FLAGS="$CMAKE_FLAGS -DLLAMA_SYCL=ON"
        CMAKE_FLAGS="$CMAKE_FLAGS -DGGML_BLAS=ON"
        CMAKE_FLAGS="$CMAKE_FLAGS -DLLAMA_OPENMP=ON"
        ;;
    *)
        echo "Usage: $0 [debug|release|cuda|vulkan|hip|metal|sycl|blas|openmp|all]"
        echo ""
        echo "Profili:"
        echo "  debug             Debug (senza ottimizzazioni)"
        echo "  release           Release CPU (default)"
        echo "  cuda              GPU NVIDIA (richiede CUDA Toolkit)"
        echo "  vulkan            GPU Vulkan (richiede driver Vulkan)"
        echo "  hip               GPU AMD ROCm"
        echo "  metal             GPU Apple Silicon"
        echo "  sycl              GPU Intel SYCL"
        echo "  blas              BLAS generico (OpenBLAS, MKL, ...)"
        echo "  openmp            OpenMP per multi-threading CPU"
        echo "  all               Tutti i backend disponibili"
        echo ""
        echo "Variabili d'ambiente:"
        echo "  CMAKE_FLAGS_EXTRA  Flag CMake aggiuntivi (es. -DLLAMA_CUDA_FA=ON)"
        echo "  BLAS_VENDOR        Vendor BLAS (Default: OpenBLAS)"
        echo "  BUILD_DIR          Directory di build (Default: ./build)"
        exit 1
        ;;
esac

# =============================================================================
# Flag extra da ambiente o file build.conf
# =============================================================================
CONF_FILE="$SCRIPT_DIR/build.conf"
if [[ -f "$CONF_FILE" ]]; then
    echo "=== Lettura build.conf ==="
    while IFS='=' read -r key value; do
        [[ "$key" =~ ^#.*$ || -z "$key" ]] && continue
        CMAKE_FLAGS="$CMAKE_FLAGS -D${key}=${value}"
    done < "$CONF_FILE"
fi

if [[ -n "${CMAKE_FLAGS_EXTRA:-}" ]]; then
    CMAKE_FLAGS="$CMAKE_FLAGS $CMAKE_FLAGS_EXTRA"
fi

# =============================================================================
# Configurazione e build
# =============================================================================
echo "=== Build: $BUILD_TYPE ==="

set -x
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" $CMAKE_FLAGS
cmake --build "$BUILD_DIR" -j "$(nproc)"
set +x

echo "=== Done: $BUILD_TYPE ==="
