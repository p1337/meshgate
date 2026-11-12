#pragma once

#include <ESPAsyncWebServer.h>
#include <StreamString.h>

// ESPAsyncWebServer's stock AsyncResponseStream allocates the complete current
// TCP send window while transmitting. That can retry forever when TLS/VPN
// fragmentation leaves enough total heap but no equally large contiguous
// block. Preserve the Print construction interface while sending the completed
// response directly in small acknowledged blocks with no send-time allocation.
class BoundedStreamResponse final : public AsyncWebServerResponse, public Print {
public:
    explicit BoundedStreamResponse(
        const char* contentType,
        size_t reserveBytes = RESPONSE_STREAM_BUFFER_SIZE
    ) {
        _code = 200;
        _contentType = contentType;
        valid_ = content_.reserve(reserveBytes);
    }

    bool _sourceValid() const override {
        return valid_ && _state < RESPONSE_END;
    }

    size_t write(const uint8_t* data, size_t len) override {
        if (_started() || !valid_) return 0;
        const size_t written = content_.write(data, len);
        _contentLength += written;
        if (written != len) valid_ = false;
        return written;
    }

    size_t write(uint8_t data) override {
        return write(&data, 1);
    }

    using Print::write;

    void _respond(AsyncWebServerRequest* request) override {
        addHeader("Connection", "close", false);
        _assembleHead(head_, request->version());
        _state = RESPONSE_HEADERS;
        _ack(request, 0, 0);
    }

    size_t _ack(AsyncWebServerRequest* request, size_t len, uint32_t time) override {
        (void)time;
        _ackedLength += len;
        inFlight_ = inFlight_ > len ? inFlight_ - len : 0;

        if (_state == RESPONSE_WAIT_ACK) {
            if (_ackedLength >= _writtenLength) _state = RESPONSE_END;
            return 0;
        }
        if (_state != RESPONSE_HEADERS && _state != RESPONSE_CONTENT) return 0;
        if (inFlight_ != 0 || !request->client()->canSend()) return 0;

        const size_t space = request->client()->space();
        if (space == 0) return 0;

        if (_state == RESPONSE_HEADERS) {
            size_t wanted = head_.length() - headOffset_;
            if (wanted > space) wanted = space;
            if (wanted > STREAM_CHUNK_BYTES) wanted = STREAM_CHUNK_BYTES;
            const size_t written = request->client()->write(
                head_.c_str() + headOffset_, wanted
            );
            if (written == 0) return 0;
            headOffset_ += written;
            _writtenLength += written;
            inFlight_ += written;
            if (headOffset_ == head_.length()) _state = RESPONSE_CONTENT;
            return written;
        }

        const size_t remaining = _contentLength - _sentLength;
        if (remaining == 0) {
            _state = RESPONSE_WAIT_ACK;
            return 0;
        }

        size_t wanted = remaining;
        if (wanted > space) wanted = space;
        if (wanted > STREAM_CHUNK_BYTES) wanted = STREAM_CHUNK_BYTES;
        const size_t written = request->client()->write(
            content_.c_str() + _sentLength, wanted
        );
        if (written == 0) return 0;

        _sentLength += written;
        _writtenLength += written;
        inFlight_ += written;
        if (_sentLength == _contentLength) _state = RESPONSE_WAIT_ACK;
        return written;
    }

private:
    static constexpr size_t STREAM_CHUNK_BYTES = 512;

    StreamString content_;
    String head_;
    size_t headOffset_ = 0;
    size_t inFlight_ = 0;
    bool valid_ = false;
};
