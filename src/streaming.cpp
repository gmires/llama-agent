#include "streaming.h"

/*
 * ============================================================================
 * StreamingBuffer: buffer thread-safe per lo streaming dei token.
 *
 * L'inferenza del modello avviene in un thread separato (o nel thread
 * principale con chiamate asincrone). I token vengono spinti in questo
 * buffer e consumati dalla UI per il rendering in tempo reale.
 *
 * La sincronizzazione è gestita tramite mutex e condition_variable.
 * ============================================================================
 */

StreamingBuffer::StreamingBuffer() = default;

void StreamingBuffer::push(const std::string & text, TokenType type)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Accumula il testo nel buffer appropriato
    switch (type) {
        case TokenType::THINKING:
            thinking_text_ += text;
            break;
        case TokenType::RESPONSE:
            response_text_ += text;
            break;
        case TokenType::UNKNOWN:
            // Se non classificato, assumiamo response
            response_text_ += text;
            break;
    }

    // Notifica il callback se registrato
    if (callback_) {
        callback_(text, type, current_turn_);
    }
}

void StreamingBuffer::finish()
{
    finished_ = true;
    cv_.notify_all();
}

void StreamingBuffer::begin_turn(int turn_index)
{
    current_turn_ = turn_index;
}

std::string StreamingBuffer::get_accumulated(TokenType type) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    switch (type) {
        case TokenType::THINKING:
            return thinking_text_;
        case TokenType::RESPONSE:
            return response_text_;
        default:
            return "";
    }
}

void StreamingBuffer::set_callback(TokenCallback cb)
{
    callback_ = std::move(cb);
}

void StreamingBuffer::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    thinking_text_.clear();
    response_text_.clear();
    finished_ = false;
}
