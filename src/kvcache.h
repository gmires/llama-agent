#ifndef LLAMA_AGENT_KVCACHE_H
#define LLAMA_AGENT_KVCACHE_H

#include "llama.h"

#include <string>
#include <vector>

enum class CacheMode {
    FAST,   // tenta llama_state_save_file/load_file, fallback su token
    TOKEN   // sempre token-file (prefill all'avvio)
};

class KVCacheManager {
public:
    KVCacheManager(const std::string & cache_dir);

    bool save(llama_context * ctx, const std::vector<llama_token> & tokens, bool prompt_only = false);
    bool load(llama_context * ctx, std::vector<llama_token> & tokens_out, bool prompt_only = false);

    bool save_state(llama_context * ctx, const std::vector<llama_token> & tokens);
    bool load_state(llama_context * ctx, std::vector<llama_token> & tokens_out);

    static std::string hash_string(const std::string & str);
    void set_cache_key(const std::string & key);
    void clear();
    std::string get_cache_path(bool prompt_only = false) const;
    std::string get_state_path() const;
    void set_enabled(bool enabled);
    void set_mode(CacheMode mode);
    const char * get_mode_name() const { return mode_ == CacheMode::FAST ? "fast" : "token"; }

private:
    std::string cache_dir_;
    std::string cache_key_;
    bool enabled_ = true;
    CacheMode mode_ = CacheMode::FAST;
};

#endif // LLAMA_AGENT_KVCACHE_H
