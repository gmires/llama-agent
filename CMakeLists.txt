cmake_minimum_required(VERSION 3.14)
project(llama-agent VERSION 0.1.0 LANGUAGES C CXX)

# =============================================================================
# Configurazione base
# =============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# =============================================================================
# Accelerazione GPU opzionale
# =============================================================================
option(LLAMA_CUDA  "Build with CUDA support"   OFF)

# =============================================================================
# Dipendenze esterne via FetchContent
# =============================================================================
include(FetchContent)

# ---- llama.cpp: runtime di inferenza ----
# Include modello, tokenizer, KVCache, sampling, e la libreria common/
# per il parsing dei parametri CLI (common_params_parse).
#
# NOTA: LLAMA_BUILD_COMMON deve essere ON per avere la libreria llama-common
# che contiene common_params_parse, common_init_from_params e altre utilita'.
set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)
if (LLAMA_CUDA)
    set(LLAMA_CUDA ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(llama
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG         master
    GIT_SHALLOW     TRUE
)
FetchContent_MakeAvailable(llama)

# ---- FTXUI: interfaccia terminale ----
# Libreria TUI funzionale: split panes, input field, colori, scroll.
# Viene linkata solo se --simple-ui non è richiesto.
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v6.1.9
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ftxui)

# =============================================================================
# Executable: llama-agent
# =============================================================================
file(GLOB LLAMA_AGENT_SOURCES src/*.cpp)

add_executable(llama-agent ${LLAMA_AGENT_SOURCES})

# Include path per i nostri sorgenti
target_include_directories(llama-agent PRIVATE src)

# Include path per le librerie llama.cpp (common/ headers come common.h, arg.h, ecc.)
target_include_directories(llama-agent PRIVATE
    ${llama_SOURCE_DIR}/common
)

# Collegamento alle librerie llama.cpp: core (llama) + utility (llama-common)
target_link_libraries(llama-agent PRIVATE
    llama
    llama-common
    ${CMAKE_THREAD_LIBS_INIT}
)

# Collegamento a FTXUI (sempre disponibile; il fallback simple-UI gestirà l'assenza)
target_link_libraries(llama-agent PRIVATE
    ftxui::component
    ftxui::dom
    ftxui::screen
)

target_compile_features(llama-agent PRIVATE cxx_std_17)
