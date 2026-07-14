#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <string_view>

namespace itch {

    // --- endianness helper: ITCH is big-endian; convert only on fields we read ---
    template <std::integral T>
    [[gnu::always_inline]] constexpr T be(T x) noexcept {
        if constexpr (std::endian::native == std::endian::little)
        {
            return std::byteswap(x);
        }
        else
        {
            return x;
        }
    }

    // 6-byte (48-bit) big-endian timestamp -> u64, MSB first (host-independent)
    [[gnu::always_inline]] constexpr uint64_t load_be48(const uint8_t* p) noexcept {
        return (uint64_t(p[0]) << 40) | (uint64_t(p[1]) << 32) | (uint64_t(p[2]) << 24) | (uint64_t(p[3]) << 16) | (uint64_t(p[4]) << 8) | uint64_t(p[5]);
    }

#pragma pack(push, 1)

    // --- 11-byte header common to every handled type ---
    struct Header {
        char msg_type;                  // off 0 - also the dispatch byte
        uint16_t stock_locate_be;       // off 1
        uint16_t tracking_be;           // off 3
        uint8_t timestamp_be[6];        // off 5

        uint16_t stock_locate() const noexcept { return be(stock_locate_be); }
        uint16_t tracking() const noexcept { return be(tracking_be); }
        uint64_t timestamp() const noexcept { return load_be48(timestamp_be); }
    };
    static_assert(sizeof(Header) == 11);

    // S = 12
    struct SystemEvent {
        Header h;
        char event_code;
    };

    // A = 36
    struct AddOrder {
        Header h;
        uint64_t order_ref_be;
        char side;  // 'B'|'S'
        uint32_t shares_be;
        char stock[8];
        uint32_t price_be; // 4 implied decimals, fixed-point
        uint64_t order_ref() const noexcept { return be(order_ref_be); }
        uint32_t shares() const noexcept { return be(shares_be); }
        uint32_t price() const noexcept { return be(price_be); }
        std::string_view sym() const noexcept { return { stock, 8 }; }
    };

    // F = 40
    struct AddOrderMPID {
        AddOrder a;
        char attribution[4];
    };

    // E = 31
    struct OrderExecuted {
        Header h;
        uint64_t order_ref_be;
        uint32_t exec_shares_be;
        uint64_t match_num_be;
        uint64_t order_ref() const noexcept { return be(order_ref_be); }
        uint32_t exec_shares() const noexcept { return be(exec_shares_be); }
        uint64_t match_num() const noexcept { return be(match_num_be); }
    };

    // C = 36
    struct OrderExecWithPrice {
        OrderExecuted e;
        char printable;
        uint32_t exec_price_be;
        uint32_t exec_price() const noexcept { return be(exec_price_be); }
    };

    // X = 23
    struct OrderCancel {
        Header h;
        uint64_t order_ref_be;
        uint32_t cancelled_shares_be;
        uint64_t order_ref() const noexcept { return be(order_ref_be); }
        uint32_t cancelled_shares() const noexcept { return be(cancelled_shares_be); }
    };

    // D = 19
    struct OrderDelete {
        Header h;
        uint64_t order_ref_be;
        uint64_t order_ref() const noexcept { return be(order_ref_be); }
    };

    // U = 35
    struct OrderReplace {
        Header h;
        uint64_t orig_ref_be;
        uint64_t new_ref_be;
        uint32_t shares_be;
        uint32_t price_be;
        uint64_t orig_ref() const noexcept { return be(orig_ref_be); }
        uint64_t new_ref() const noexcept { return be(new_ref_be); }
        uint32_t shares() const noexcept { return be(shares_be); }
        uint32_t price() const noexcept { return be(price_be); }
    };

    // P = 44
    struct TradeNonCross {
        Header h;
        uint64_t order_ref_be;
        char side;
        uint32_t shares_be;
        char stock[8];
        uint32_t price_be;
        uint64_t match_num_be;
        uint64_t order_ref() const noexcept { return be(order_ref_be); }
        uint32_t shares() const noexcept { return be(shares_be); }
        uint32_t price() const noexcept { return be(price_be); }
        uint64_t match_num() const noexcept { return be(match_num_be); }
        std::string_view sym() const noexcept { return { stock, 8 }; }
    };

#pragma pack(pop)

    // --- single source of truth for wire lengths (framing walk + asserts) ---
    constexpr uint16_t payload_len(char t) noexcept {
        switch (t) {
        case 'S': return 12;
        case 'R': return 39; 
        case 'A': return 36;
        case 'F': return 40; 
        case 'E': return 31; 
        case 'C': return 36;
        case 'X': return 23; 
        case 'D': return 19; 
        case 'U': return 35;
        case 'P': return 44; 
        case 'H': return 25; 
        case 'Y': return 20;
        default:  return 0;   // skip types: advance by on-wire prefix, not this
        }
    }

    // --- padding tripwires: fire if [[gnu::packed]] or a field type is wrong ---
    static_assert(sizeof(SystemEvent) == payload_len('S'));
    static_assert(sizeof(AddOrder) == payload_len('A'));
    static_assert(sizeof(AddOrderMPID) == payload_len('F'));
    static_assert(sizeof(OrderExecuted) == payload_len('E'));
    static_assert(sizeof(OrderExecWithPrice) == payload_len('C'));
    static_assert(sizeof(OrderCancel) == payload_len('X'));
    static_assert(sizeof(OrderDelete) == payload_len('D'));
    static_assert(sizeof(OrderReplace) == payload_len('U'));
    static_assert(sizeof(TradeNonCross) == payload_len('P'));
    // reorder tripwires: one offset per struct
    static_assert(offsetof(AddOrder, price_be) == 32);
    static_assert(offsetof(OrderExecuted, match_num_be) == 23);
    static_assert(offsetof(OrderReplace, price_be) == 31);
    static_assert(offsetof(TradeNonCross, match_num_be) == 36);

} // namespace itch