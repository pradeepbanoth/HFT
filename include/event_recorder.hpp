#pragma once
// event_recorder.hpp — deterministic binary market-event recorder/replayer

#include "types.hpp"
#include "simulator.hpp"

#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>

namespace hft {

enum class RecordedEventType : uint8_t {
    L2 = 1,
    L3 = 2,
    Trade = 3
};

struct RecorderHeader {
    char magic[8] = {'H','F','T','R','E','C','1','\0'};
    uint32_t version = 1;
    uint32_t reserved = 0;
};

struct EventRecordHeader {
    RecordedEventType type;
    uint32_t payload_size;
    int64_t timestamp;
};

struct PackedL2 {
        char symbol[24];
        BookSide side;
        double price;
        double qty;
        int64_t timestamp;
        int64_t seq;
    };

    struct PackedL3 {
        char symbol[24];
        char order_id[48];
        L3Event event;
        Side side;
        double price;
        double qty;
        int64_t timestamp;
        int64_t seq;
    };

    struct PackedTrade {
        char symbol[24];
        char trade_id[48];
        Side side;
        double price;
        double qty;
        int64_t timestamp;
        Side aggressor;
        bool buyer_mm;
    };

class EventRecorder {
public:
    explicit EventRecorder(const std::string& path)
        : out_(path, std::ios::binary)
    {
        if (!out_) throw std::runtime_error("EventRecorder: cannot open " + path);
        RecorderHeader h;
        out_.write(reinterpret_cast<const char*>(&h), sizeof(h));
    }

    void write(const MarketEvent& evt) {
        std::visit([&](const auto& e) {
            write_impl(e);
        }, evt);
        ++count_;
    }

    int64_t count() const noexcept {
        return count_;
    }

private:
    std::ofstream out_;
    int64_t count_ = 0;

    template<typename T>
    void write_raw(RecordedEventType type, const T& payload, int64_t ts) {
        EventRecordHeader rh;
        rh.type = type;
        rh.payload_size = sizeof(T);
        rh.timestamp = ts;

        out_.write(reinterpret_cast<const char*>(&rh), sizeof(rh));
        out_.write(reinterpret_cast<const char*>(&payload), sizeof(T));
    }

    void write_impl(const L2Update& e) {
        PackedL2 p;
        assign_symbol(p.symbol, e.symbol);
        p.side = e.side;
        p.price = e.price;
        p.qty = e.qty;
        p.timestamp = e.timestamp;
        p.seq = e.seq;
        write_raw(RecordedEventType::L2, p, e.timestamp);
    }

    void write_impl(const L3Update& e) {
        PackedL3 p;
        assign_symbol(p.symbol, e.symbol);
        assign_id(p.order_id, e.order_id);
        p.event = e.event;
        p.side = e.side;
        p.price = e.price;
        p.qty = e.qty;
        p.timestamp = e.timestamp;
        p.seq = e.seq;
        write_raw(RecordedEventType::L3, p, e.timestamp);
    }

    void write_impl(const Trade& e) {
        PackedTrade p;
        assign_symbol(p.symbol, e.symbol);
        assign_id(p.trade_id, e.trade_id);
        p.side = e.side;
        p.price = e.price;
        p.qty = e.qty;
        p.timestamp = e.timestamp;
        p.aggressor = e.aggressor;
        p.buyer_mm = e.buyer_mm;
        write_raw(RecordedEventType::Trade, p, e.timestamp);
    }

    static void assign_symbol(char dst[24], const std::string& s) {
        std::fill(dst, dst + 24, '\0');
        std::copy_n(s.data(), std::min<size_t>(s.size(), 23), dst);
    }

    static void assign_id(char dst[48], const std::string& s) {
        std::fill(dst, dst + 48, '\0');
        std::copy_n(s.data(), std::min<size_t>(s.size(), 47), dst);
    }

    
};

class EventReplayReader {
public:
    explicit EventReplayReader(const std::string& path)
        : in_(path, std::ios::binary)
    {
        if (!in_) throw std::runtime_error("EventReplayReader: cannot open " + path);

        RecorderHeader h;
        in_.read(reinterpret_cast<char*>(&h), sizeof(h));

        if (!in_ || std::string(h.magic, h.magic + 6) != "HFTREC") {
            throw std::runtime_error("EventReplayReader: invalid file format");
        }
    }

    bool next(MarketEvent& out) {
        EventRecordHeader rh;
        in_.read(reinterpret_cast<char*>(&rh), sizeof(rh));
        if (!in_) return false;

        switch (rh.type) {
            case RecordedEventType::L2: {
                 PackedL2 p;
                read_payload(p, rh.payload_size);
                out = L2Update{
                    std::string(p.symbol),
                    p.side,
                    p.price,
                    p.qty,
                    p.timestamp,
                    p.seq
                };
                return true;
            }

            case RecordedEventType::L3: {
                  PackedL3 p;
                read_payload(p, rh.payload_size);
                out = L3Update{
                    std::string(p.symbol),
                    p.event,
                    std::string(p.order_id),
                    p.side,
                    p.price,
                    p.qty,
                    p.timestamp,
                    p.seq
                };
                return true;
            }

            case RecordedEventType::Trade: {
                    PackedTrade p;
                read_payload(p, rh.payload_size);
                out = Trade{
                    std::string(p.trade_id),
                    std::string(p.symbol),
                    p.side,
                    p.price,
                    p.qty,
                    p.timestamp,
                    p.aggressor,
                    p.buyer_mm
                };
                return true;
            }

            default:
                throw std::runtime_error("EventReplayReader: unknown event type");
        }
    }

private:
    std::ifstream in_;

    template<typename T>
    void read_payload(T& payload, uint32_t size) {
        if (size != sizeof(T)) {
            throw std::runtime_error("EventReplayReader: payload size mismatch");
        }
        in_.read(reinterpret_cast<char*>(&payload), sizeof(T));
        if (!in_) throw std::runtime_error("EventReplayReader: truncated payload");
    }
};

} // namespace hft