#include "kvcache.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

/*
 * ============================================================================
 * KVCacheManager: salva e carica la KVCache su disco.
 *
 * Salva solo i TOKEN su file (non lo stato completo del contesto).
 * Al caricamento, i token vengono restituiti e il chiamante ricostruisce
 * la KVCache valutandoli uno per uno. Questo evita problemi di
 * compatibilità del formato di serializzazione interna di llama.cpp.
 *
 * Formato file: header (magic + version) + count(uint32) + tokens[]
 * ============================================================================
 */

static const uint32_t CACHE_MAGIC   = 0x4C4C4147; // "LLAG"
static const uint32_t CACHE_VERSION = 1;

KVCacheManager::KVCacheManager(const std::string & cache_dir)
    : cache_dir_(cache_dir)
{
    if (!cache_dir_.empty()) {
        fs::create_directories(cache_dir_);
    }
}

bool KVCacheManager::save(llama_context * /*ctx*/, const std::vector<llama_token> & tokens, bool prompt_only)
{
    if (!enabled_) return false;
    const std::string path = get_cache_path(prompt_only);
    if (path.empty()) return false;

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "[KVCache] Errore apertura file: %s\n", path.c_str());
        return false;
    }

    file.write((const char*)&CACHE_MAGIC, sizeof(CACHE_MAGIC));
    file.write((const char*)&CACHE_VERSION, sizeof(CACHE_VERSION));
    uint32_t count = (uint32_t)tokens.size();
    file.write((const char*)&count, sizeof(count));
    file.write((const char*)tokens.data(), tokens.size() * sizeof(llama_token));

    if (!file.good()) {
        fprintf(stderr, "[KVCache] Errore scrittura file: %s\n", path.c_str());
        return false;
    }

    fprintf(stderr, "[KVCache] Salvati %zu token -> %s\n", tokens.size(), path.c_str());
    return true;
}

bool KVCacheManager::load(llama_context * /*ctx*/, std::vector<llama_token> & tokens_out, bool prompt_only)
{
    if (!enabled_) return false;
    const std::string path = get_cache_path(prompt_only);
    if (!fs::exists(path)) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "[KVCache] Errore apertura file: %s\n", path.c_str());
        return false;
    }

    uint32_t magic, version, count;
    file.read((char*)&magic, sizeof(magic));
    file.read((char*)&version, sizeof(version));
    file.read((char*)&count, sizeof(count));

    if (magic != CACHE_MAGIC || version != CACHE_VERSION) {
        fprintf(stderr, "[KVCache] Formato non riconosciuto: magic=%08x ver=%u\n", magic, version);
        return false;
    }

    tokens_out.resize(count);
    file.read((char*)tokens_out.data(), count * sizeof(llama_token));

    if (!file.good()) {
        fprintf(stderr, "[KVCache] Errore lettura file: %s\n", path.c_str());
        tokens_out.clear();
        return false;
    }

    fprintf(stderr, "[KVCache] Caricati %zu token da %s\n", tokens_out.size(), path.c_str());
    return true;
}

std::string KVCacheManager::hash_string(const std::string & str)
{
    unsigned long hash = 5381;
    for (char c : str) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(c);
    }
    std::stringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return ss.str();
}

void KVCacheManager::set_cache_key(const std::string & key)
{
    cache_key_ = key;
}

void KVCacheManager::clear()
{
    const std::string path = get_cache_path();
    if (!path.empty() && fs::exists(path)) {
        fs::remove(path);
        fprintf(stderr, "[KVCache] Cache eliminata: %s\n", path.c_str());
    }
}

std::string KVCacheManager::get_cache_path(bool prompt_only) const
{
    if (cache_dir_.empty()) return "";
    const std::string key = cache_key_.empty() ? "default" : cache_key_;
    const std::string suffix = prompt_only ? "_prompt" : "";
    const std::string filename = "cache_" + hash_string(key) + suffix + ".bin";
    return (fs::path(cache_dir_) / filename).string();
}

void KVCacheManager::set_enabled(bool enabled)
{
    enabled_ = enabled;
}
