// Test: La codifica/decodifica dei token per il tool calling JSON
// Verifica che common_token_to_piece non perda caratteri (es. primo char di ogni parola)
#include "llama.h"
#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <iostream>

int main(int argc, char ** argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <vocab.gguf>\n", argv[0]);
        return 1;
    }

    std::setlocale(LC_NUMERIC, "C");
    common_init();

    llama_backend_init();

    common_params params;
    params.model.path = argv[1];

    auto init = common_init_from_params(params);
    if (!init) {
        fprintf(stderr, "Failed to load vocab\n");
        return 1;
    }

    auto * model = init->model();
    auto * vocab = llama_model_get_vocab(model);

    // Test tool call JSON
    std::vector<std::string> test_strings = {
        // Base tool call
        "{\"tool\": \"write\", \"args\": {\"path\": \"script.js\", \"content\": \"hi\"}}",

        // JS code with braces
        "{\"tool\": \"write\", \"args\": {\"path\": \"snake.js\", \"content\": \"let snake = [{ x: 10, y: 10 }];\"}}",

        // Partial JSON (simula output streaming non ancora completo)
        "{\"tool\": \"write\", \"args\": {\"path\": \"s",

        // JSON keys - test each key word
        "\"tool\": \"write\"",
        "\"write\"",

        // Tool call in markdown
        "```json\n{\"tool\": \"write\", \"args\": {\"path\": \"x.js\"}}\n```",
    };

    for (const auto & test_str : test_strings) {
        printf("=== INPUT: '%s' ===\n", test_str.c_str());

        // Tokenize
        auto tokens = common_tokenize(vocab, test_str, false, false);

        printf("Tokens (%zu):\n", tokens.size());
        for (size_t i = 0; i < tokens.size(); i++) {
            std::string piece = common_token_to_piece(vocab, tokens[i], false);
            printf("  [%4zu] id=%-6d piece='%s'\n", i, tokens[i], piece.c_str());
        }

        // Reconstruct
        std::string reconstructed;
        for (auto t : tokens) {
            reconstructed += common_token_to_piece(vocab, t, false);
        }

        printf("Reconstructed: '%s'\n", reconstructed.c_str());
        printf("Match: %s\n\n", reconstructed == test_str ? "YES" : "NO");

        if (reconstructed != test_str) {
            printf("  *** MISMATCH ***\n");
            printf("  Original:  '%s'\n", test_str.c_str());
            printf("  Got:       '%s'\n", reconstructed.c_str());
            // Show diff
            for (size_t i = 0; i < test_str.size() && i < reconstructed.size(); i++) {
                if (test_str[i] != reconstructed[i]) {
                    printf("  First diff at pos %zu: expected '%c'(0x%02x) got '%c'(0x%02x)\n",
                           i, test_str[i], (unsigned char)test_str[i],
                           reconstructed[i], (unsigned char)reconstructed[i]);
                    break;
                }
            }
            if (test_str.size() != reconstructed.size()) {
                printf("  Length diff: expected %zu got %zu\n",
                       test_str.size(), reconstructed.size());
                if (reconstructed.size() > test_str.size()) {
                    printf("  Extra chars at end: '%s'\n",
                           reconstructed.substr(test_str.size()).c_str());
                }
            }
        }
    }

    llama_backend_free();
    return 0;
}
