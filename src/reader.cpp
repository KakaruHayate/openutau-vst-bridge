#include "reader.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace bridge {
namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

ReadStatus FrameReader::ReadLine(std::string *line, int timeoutMs) {
    int64_t deadline = NowMs() + timeoutMs;
    while (true) {
        const uint8_t *begin = buffer_.data() + start_;
        const uint8_t *found = end_ > start_
            ? static_cast<const uint8_t *>(std::memchr(begin, '\n', end_ - start_))
            : nullptr;
        if (found != nullptr) {
            size_t length = static_cast<size_t>(found - begin);
            // Tolerate CRLF from hand-written or Windows-side test peers, as the other side does.
            if (length > 0 && begin[length - 1] == '\r') {
                length--;
            }
            line->assign(reinterpret_cast<const char *>(begin), length);
            start_ += static_cast<size_t>(found - begin) + 1;
            return ReadStatus::Ok;
        }
        if (end_ - start_ >= kMaxLineBytes) {
            error_ = "Control line exceeded " + std::to_string(kMaxLineBytes) +
                     " bytes without a newline.";
            return ReadStatus::ProtocolError;
        }
        int64_t remaining = deadline - NowMs();
        if (remaining <= 0) {
            return ReadStatus::Timeout;
        }
        ReadStatus filled = Fill(static_cast<int>(std::min<int64_t>(remaining, 1000)));
        if (filled == ReadStatus::EndOfStream && end_ > start_) {
            error_ = "Stream ended mid-line with " + std::to_string(end_ - start_) +
                     " bytes buffered.";
            return ReadStatus::ProtocolError;
        }
        if (filled != ReadStatus::Ok && filled != ReadStatus::Timeout) {
            return filled;
        }
    }
}

ReadStatus FrameReader::ReadExactly(size_t count, std::vector<uint8_t> *payload) {
    payload->resize(count);
    size_t copied = 0;
    int64_t deadline = NowMs() + kPayloadTimeoutMs;
    while (copied < count) {
        size_t available = std::min(end_ - start_, count - copied);
        if (available > 0) {
            std::memcpy(payload->data() + copied, buffer_.data() + start_, available);
            start_ += available;
            copied += available;
            continue;
        }
        int64_t remaining = deadline - NowMs();
        if (remaining <= 0) {
            error_ = "Stalled after " + std::to_string(copied) + " of " +
                     std::to_string(count) + " payload bytes.";
            return ReadStatus::ProtocolError;
        }
        ReadStatus filled = Fill(static_cast<int>(std::min<int64_t>(remaining, 1000)));
        if (filled == ReadStatus::EndOfStream) {
            // A short read here is unrecoverable: the stream position is lost (§8).
            error_ = "Stream ended after " + std::to_string(copied) + " of " +
                     std::to_string(count) + " payload bytes.";
            return ReadStatus::ProtocolError;
        }
        if (filled != ReadStatus::Ok && filled != ReadStatus::Timeout) {
            return filled;
        }
    }
    return ReadStatus::Ok;
}

ReadStatus FrameReader::Fill(int timeoutMs) {
    if (start_ == end_) {
        start_ = 0;
        end_ = 0;
    } else if (end_ == buffer_.size()) {
        if (start_ > 0) {
            std::memmove(buffer_.data(), buffer_.data() + start_, end_ - start_);
            end_ -= start_;
            start_ = 0;
        } else {
            buffer_.resize(buffer_.size() * 2);
        }
    }
    int read = stream_->Read(buffer_.data() + end_, buffer_.size() - end_, timeoutMs);
    if (read == 0) {
        return ReadStatus::Timeout;
    }
    if (read == kStreamEnded) {
        return ReadStatus::EndOfStream;
    }
    if (read < 0) {
        error_ = "Stream read failed.";
        return ReadStatus::ProtocolError;
    }
    end_ += static_cast<size_t>(read);
    return ReadStatus::Ok;
}

}  // namespace bridge
