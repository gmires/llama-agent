#ifndef LLAMA_AGENT_KVCACHE_H
#define LLAMA_AGENT_KVCACHE_H

#include "llama.h"

#include <string>
#include <vector>

/**
 * Gestore della KVCache persistente su disco.
 *
 * Salva e carica lo stato della KVCache (insieme a logits ed embedding)
 * nella directory `.cache/`. All'avvio, se il file esiste, la cache viene
 * ripristinata per preservare il contesto tra sessioni.
 */
class KVCacheManager {
public:
    /**
     * Costruttore.
     * @param cache_dir Directory dove salvare i file di cache (es. ".cache")
     */
    KVCacheManager(const std::string & cache_dir);

    /**
     * Salva lo stato completo del contesto su file.
     * @param ctx Puntatore al contesto llama
     * @param tokens Token della conversazione (salvati come metadati)
     * @param prompt_only Se true, salva come cache del solo prompt (per /regen)
     * @return true se il salvataggio è riuscito
     */
    bool save(llama_context * ctx, const std::vector<llama_token> & tokens, bool prompt_only = false);

    /**
     * Carica lo stato completo del contesto da file.
     * @param ctx Puntatore al contesto llama (già inizializzato)
     * @param tokens_out Vettore dove verranno copiati i token caricati
     * @param prompt_only Se true, carica la cache del solo prompt (per /regen)
     * @return true se il caricamento è riuscito, false se il file non esiste
     */
    bool load(llama_context * ctx, std::vector<llama_token> & tokens_out, bool prompt_only = false);

    /**
     * Calcola l'hash di una stringa (usato per identificare la cache).
     * Cache diverse per modelli/prompt diversi.
     */
    static std::string hash_string(const std::string & str);

    /**
     * Imposta una chiave di hash personalizzata per distinguere le cache.
     */
    void set_cache_key(const std::string & key);

    /**
     * Elimina il file di cache corrente.
     */
    void clear();

    /**
     * Restituisce il percorso completo del file di cache.
     * @param prompt_only Se true, restituisce il percorso per la cache prompt-only
     */
    std::string get_cache_path(bool prompt_only = false) const;

    /**
     * Disabilita la cache (modalità read-only / no-cache).
     */
    void set_enabled(bool enabled);

private:
    std::string cache_dir_;
    std::string cache_key_;
    bool enabled_ = true;
};

#endif // LLAMA_AGENT_KVCACHE_H
