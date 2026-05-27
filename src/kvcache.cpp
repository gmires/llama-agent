#include "kvcache.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

static const uint32_t CACHE_MAGIC   = 0x4C4C4147;
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
        fprintf(stderr, "[KVCache] Errore apertura token file: %s\n", path.c_str());
        return false;
    }

    file.write((const char*)&CACHE_MAGIC, sizeof(CACHE_MAGIC));
    file.write((const char*)&CACHE_VERSION, sizeof(CACHE_VERSION));
    uint32_t count = (uint32_t)tokens.size();
    file.write((const char*)&count, sizeof(count));
    file.write((const char*)tokens.data(), tokens.size() * sizeof(llama_token));

    if (!file.good()) {
        fprintf(stderr, "[KVCache] Errore scrittura token file: %s\n", path.c_str());
        return false;
    }

    return true;
}

bool KVCacheManager::load(llama_context * /*ctx*/, std::vector<llama_token> & tokens_out, bool prompt_only)
{
    if (!enabled_) return false;
    const std::string path = get_cache_path(prompt_only);
    if (!fs::exists(path)) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    uint32_t magic, version, count;
    file.read((char*)&magic, sizeof(magic));
    file.read((char*)&version, sizeof(version));
    file.read((char*)&count, sizeof(count));

    if (magic != CACHE_MAGIC || version != CACHE_VERSION) {
        return false;
    }

    tokens_out.resize(count);
    file.read((char*)tokens_out.data(), count * sizeof(llama_token));

    if (!file.good()) {
        tokens_out.clear();
        return false;
    }

    fprintf(stderr, "[KVCache] Caricati %zu token da %s\n", tokens_out.size(), path.c_str());
    return true;
}

bool KVCacheManager::save_state(llama_context * ctx, const std::vector<llama_token> & tokens)
{
    if (!enabled_) return false;
    if (mode_ != CacheMode::FAST) return false;
    const std::string path = get_state_path();
    if (path.empty()) return false;

    bool ok = llama_state_save_file(ctx, path.c_str(), tokens.data(), tokens.size());
    if (ok) {
        fprintf(stderr, "[KVCache] Stato salvato (%zu token, %zu bytes) -> %s\n",
                tokens.size(), llama_state_get_size(ctx), path.c_str());
    }
    return ok;
}

bool KVCacheManager::load_state(llama_context * ctx, std::vector<llama_token> & tokens_out)
{
    if (!enabled_) return false;
    if (mode_ != CacheMode::FAST) return false;
    const std::string path = get_state_path();
    if (!fs::exists(path)) return false;

    size_t n_token_capacity = 65536;
    tokens_out.resize(n_token_capacity);
    size_t n_token_count = 0;

    bool ok = llama_state_load_file(ctx, path.c_str(), tokens_out.data(), n_token_capacity, &n_token_count);
    if (ok) {
        tokens_out.resize(n_token_count);
        fprintf(stderr, "[KVCache] Stato caricato (%zu token, %zu bytes) <- %s\n",
                n_token_count, llama_state_get_size(ctx), path.c_str());
    } else {
        tokens_out.clear();
        fprintf(stderr, "[KVCache] Stato non valido, eliminazione: %s\n", path.c_str());
        fs::remove(path);
    }
    return ok;
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
    // Elimina token file
    {
        const std::string path = get_cache_path();
        if (!path.empty() && fs::exists(path)) {
            fs::remove(path);
        }
    }
    // Elimina prompt file
    {
        const std::string path = get_cache_path(true);
        if (!path.empty() && fs::exists(path)) {
            fs::remove(path);
        }
    }
    // Elimina state file
    {
        const std::string path = get_state_path();
        if (!path.empty() && fs::exists(path)) {
            fs::remove(path);
        }
    }

    fprintf(stderr, "[KVCache] Cache eliminata\n");
}

std::string KVCacheManager::get_cache_path(bool prompt_only) const
{
    if (cache_dir_.empty()) return "";
    const std::string key = cache_key_.empty() ? "default" : cache_key_;
    const std::string suffix = prompt_only ? "_prompt" : "";
    const std::string filename = "cache_" + hash_string(key) + suffix + ".bin";
    return (fs::path(cache_dir_) / filename).string();
}

std::string KVCacheManager::get_state_path() const
{
    if (cache_dir_.empty()) return "";
    const std::string key = cache_key_.empty() ? "default" : cache_key_;
    const std::string filename = "cache_" + hash_string(key) + "_state.bin";
    return (fs::path(cache_dir_) / filename).string();
}

void KVCacheManager::set_enabled(bool enabled)
{
    enabled_ = enabled;
}

void KVCacheManager::set_mode(CacheMode mode)
{
    mode_ = mode;
}
