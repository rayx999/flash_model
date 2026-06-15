#pragma once

#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <concepts>
#include <cstdint>
#include <optional>
#include <array>
#include <string_view>

// Add this line immediately below the enum definition!
// It tells magic_enum to scan all values from 0 up to 255 at compile time.
#define MAGIC_ENUM_RANGE_MIN 0
#define MAGIC_ENUM_RANGE_MAX 255
#include <magic_enum.hpp>

// Helper for std::format missing in C++20, you can replace this with fmt::format if you have the fmt library available
#include <sstream>
#include <utility>

namespace {
    template <typename... Args>
    std::string make_msg(const char* fmt, Args&&... args) {
        // 1. Probe the exact buffer length required (returns number of characters without \0)
        int size = std::snprintf(nullptr, 0, fmt, std::forward<Args>(args)...);
        
        if (size <= 0) {
            return "Formatting Error";
        }

        // 2. Allocate a string buffer dynamically to fit the message perfectly
        std::string result(size, '\0');
        
        // 3. Write securely directly into the string's internal array (size + 1 includes \0)
        std::snprintf(result.data(), size + 1, fmt, std::forward<Args>(args)...);
        
        return result;
    }
}

// The Hardware Specification Profile layout
// ------------------------------------------
struct FlashDeviceProfile {
    std::string_view model_name;
    std::array<uint8_t, 3> jedec_id;
    std::array<uint8_t, 256> sfdp;
    size_t capacity_bytes;
    uint8_t default_address_bytes;
};

// C++20 Concept to validate that a policy type matches the Flash specifications
template <typename T>
concept ValidFlashDevice = requires {
    // 1. Enforce that T contains a static member called 'profile'
    { T::profile } -> std::convertible_to<const FlashDeviceProfile&>;
} && 
// 2. Enforce that it can be evaluated at compile-time (constexpr check)
requires {
    typename std::integral_constant<size_t, T::profile.capacity_bytes>;
    typename std::integral_constant<uint8_t, T::profile.default_address_bytes>;
};

// Define specific vendor hardware variations as compile-time constants
struct MT25QU02GCBB {
    static constexpr FlashDeviceProfile profile = {
        .model_name = "Micron_MT25QU02GCBB",
        .jedec_id = {0x20, 0xBB, 0x22}, // Matches your required 0x20, 0xBB, 0x22 values
        .sfdp = { 'S', 'F', 'D', 'P' }, // You can populate this with actual SFDP data if needed
        .capacity_bytes = 256ULL * 1024 * 1024, // 2Gb Density (256MB)
        .default_address_bytes = 4 // 2Gb density defaults to 4-byte address tracking
    };
};

// Enforce valid Micron Flash address widths (3 bytes or 4 bytes only)
template <size_t Width>
concept ValidAddressMode = (Width == 3) || (Width == 4);

// Enforce valid dummy clock ranges based on Micron hardware restrictions
template <uint8_t Cycles>
concept ValidDummyCycles = (Cycles >= 1) && (Cycles <= 14);

// Special concept for commands that do not use addresses/dummy clocks (like Reset)
template <size_t Width, uint8_t Cycles>
concept ValidControlCommand = (Width == 0) && (Cycles == 0);

enum class FlashCmd : uint8_t {
    ResetEnable = 0x66, // RESET ENABLE Command
    ResetMemory = 0x99, // RESET MEMORY Command
    ReadId      = 0x9F, // JEDEC READ ID Command
    ReadSFDP    = 0x5A, // READ SFDP Command
    FastRead    = 0x0B, // 1-1-1 Fast Read Command
    Read4Byte   = 0x13  // 4-BYTE READ Command
};
constexpr bool is_valid_flash_cmd(FlashCmd cmd) {
    // magic_enum automatically scans the enum and checks if 'cmd' matches a valid identifier
    return magic_enum::enum_contains(cmd);
}

enum class BusProtocol : uint8_t { Extended_SPI, Dual_SPI, Quad_SPI };
constexpr bool is_valid_bus_protocol(BusProtocol prot) {
    return magic_enum::enum_contains(prot);
}

// Coomands traits structure and compile-time command matrix
// ---------------------------------------------------------
struct CommandTraits {
    FlashCmd cmd;
    BusProtocol protocol;
    uint8_t address_bytes;
    uint8_t dummy_clocks;

    // You must explicitly provide a default constructor so the std::array can initialize empty slots
    constexpr CommandTraits() : cmd(static_cast<FlashCmd>(0)), protocol(BusProtocol::Extended_SPI), address_bytes(0), dummy_clocks(0) {}

    // A Templated Factory Function that intercepts variables and forces concept validation
    template <uint8_t AddrBytes, uint8_t DummyClocks>
    requires (ValidAddressMode<AddrBytes> && ValidDummyCycles<DummyClocks>) || ValidControlCommand<AddrBytes, DummyClocks>
    static constexpr CommandTraits create(FlashCmd c, BusProtocol p) {
        if (!is_valid_flash_cmd(c)) {
            throw "Error: Attempted to create CommandTraits with an unassigned raw command ID!";
        }
        
        if (!is_valid_bus_protocol(p)) {
            throw "Error: Attempted to create CommandTraits with an invalid bus protocol!";
        }

        return CommandTraits{c, p, AddrBytes, DummyClocks};
    } 

private:
    // Private backing constructor to prevent un-validated structures
    constexpr CommandTraits(FlashCmd c, BusProtocol p, uint8_t a, uint8_t d)
        : cmd(c), protocol(p), address_bytes(a), dummy_clocks(d) {}
};

// A flat array containing exactly 256 command entries
// Lives in contiguous memory, meaning perfect CPU L1-Cache utilization
inline constexpr std::array<CommandTraits, 256> CommandMatrix = []() {
    std::array<CommandTraits, 256> table{};

    // Define a local helper lambda right here!
    // It captures nothing, takes an enum, and converts it to its raw index size_t.
    auto idx = [](FlashCmd cmd) constexpr { return static_cast<size_t>(cmd); };

    // Initialize array at compile-time...
    table[idx(FlashCmd::ResetEnable)] = CommandTraits::create<0, 0>(FlashCmd::ResetEnable, BusProtocol::Extended_SPI);
    table[idx(FlashCmd::ResetMemory)] = CommandTraits::create<0, 0>(FlashCmd::ResetMemory, BusProtocol::Extended_SPI);
    table[idx(FlashCmd::ReadId)]      = CommandTraits::create<0, 0>(FlashCmd::ReadId,      BusProtocol::Extended_SPI);
    table[idx(FlashCmd::ReadSFDP)]    = CommandTraits::create<3, 8>(FlashCmd::ReadSFDP,    BusProtocol::Extended_SPI);
    table[idx(FlashCmd::FastRead)]    = CommandTraits::create<3, 8>(FlashCmd::FastRead,    BusProtocol::Extended_SPI);
    return table;
}();

// Pure O(1) runtime lookup tool for your SystemC b_transport method
inline std::optional<CommandTraits> get_traits(FlashCmd target) noexcept {
    auto idx = static_cast<size_t>(target);
    if (idx >= CommandMatrix.size()) [[unlikely]] {
        return std::nullopt; // Out-of-bounds command code, immediately return no traits
    }

    const auto& traits = CommandMatrix[idx];
    if (traits.cmd == target) [[likely]] {
        return traits; // Valid command found, return its traits
    } else {
        return std::nullopt; // No valid command traits found for this target
    }
}

// The main FlashModel SystemC module definition
// ---------------------------------------------
template <typename DevicePolicy>
requires ValidFlashDevice<DevicePolicy>
class FlashModel : public sc_core::sc_module {
public:
    // TLM 2.0 target socket initialization
    tlm_utils::simple_target_socket<FlashModel<DevicePolicy>, 32> flash_socket;

    SC_HAS_PROCESS(FlashModel);
    
    explicit FlashModel(sc_core::sc_module_name name) 
        : sc_module(name), flash_socket("flash_socket") 
    {
        // Bind the transaction function using C++11/C++14 style lambdas cleanly
        flash_socket.register_b_transport(this, &FlashModel<DevicePolicy>::b_transport);
    }

private:
    // utility
    uint32_t get_addr3(uint8_t* stream) const noexcept {
        return (stream[0] << 16) | (stream[1] << 8) | stream[2];
    }

    uint32_t get_addr4(uint8_t* stream) const noexcept {
        return (stream[0] << 24) | (stream[1] << 16) | (stream[2] << 8) | stream[3];
    }

    // Internal registers matching the hardware specification state machines
    bool m_reset_enabled{false};

    // Core TLM blocking transport implementation method
    void b_transport(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) noexcept;

    // Command controller execution unit
    int process_flash_cmd(CommandTraits& traits, uint8_t* stream, unsigned int len) noexcept;

    int read_id(uint8_t* stream, unsigned int len) noexcept;
    int read_sfdp(uint8_t* stream, unsigned int len, uint32_t dummy_clocks) noexcept;
    int read_flash(uint8_t* stream, unsigned int len) noexcept;
};

