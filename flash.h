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

#include "flash_storage.h"

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
struct FlashDeviceConfig {
    uint16_t dummy_cycles    : 4; // 1111 = default dummy for each read command
    uint16_t xip_mode        : 3; // 111 = disable (default)   
    uint16_t driver_strength : 3; // 111 = 30ohms (default)
    uint16_t transfer_rate   : 1; // 0 = enable (DTR), 1 = disable (STR default)
    uint16_t hold            : 1; // 0 = enabled, 1 = disabled (default)
    uint16_t quad_io         : 1; // 0 = enabled, 1 = disabled (default)
    uint16_t dual_io         : 1; // 0 = enabled, 1 = disabled (default)
    uint16_t select_128Mb    : 1; // 0 = highest 128Mb segment, 1 = lowest 128Mb segment (default)
    uint16_t addr_len        : 1; // 0 = 4-byte address, 1 = 3-byte address (default)
};

struct FlashDeviceProfile {
    std::string_view model_name;
    std::array<uint8_t, 3> jedec_id;
    std::array<uint8_t, 256> sfdp;
    uint32_t sector_num;
    uint32_t sector_size;
    uint32_t subsector_32KB_num; 
    uint32_t subsector_4KB_num;
    uint32_t die_num;
    FlashDeviceConfig config;
};

enum class FlashCmd : uint8_t {
    ResetEnable = 0x66, // RESET ENABLE Command
    ResetMemory = 0x99, // RESET MEMORY Command
    ReadId      = 0x9F, // JEDEC READ ID Command
    ReadSFDP    = 0x5A, // READ SFDP Command
    WriteEnable = 0x06, // WRITE ENABLE Command
    WriteDisable= 0x04, // WRITE DISABLE Command
    Enter4Byte  = 0xB7, // ENTER 4-BYTE ADDRESS MODE Command
    Exit4Byte   = 0xE9, // EXIT 4-BYTE ADDRESS MODE Command
    // Read category commands
    Read        = 0x03, // 1-1-1 Read Command
    FastRead    = 0x0B, // 1-1-1, 2-2-2, 4-4-4 Fast Read Command
    DtrFastRead = 0x0D, // 1-1-1, 2-2-2, 4-4-4 Fast Read Command
    QuadOutputFastRead = 0x6B, // 1-1-4, 4-4-4 Quad Output Fast Read Command    
    DtrQuadOutputFastRead = 0x6D, // 1-1-4, 4-4-4 Quad Output Fast Read Command    
    QuadInputOutputFastRead = 0xEB, // 1-4-4, 4-4-4 Quad Input/Output Fast Read Command    
    DtrQuadInputOutputFastRead = 0xED, // 1-4-4, 4-4-4 Quad Input/Output Fast Read Command    
    Read4Byte   = 0x13,  // 4-BYTE READ Command, 1-1-1
    FastRead4Byte = 0x0C,  // 4-BYTE Fast Read Command, 1-1-1 2-2-2 4-4-4
    DtrFastRead4Byte = 0x0E,  // 4-BYTE Fast Read Command DTR, 1-1-1 2-2-2 4-4-4    
    QuadOutputFastRead4Byte = 0x6C,  // 4-BYTE Quad Output Fast Read Command, 1-1-4 4-4-4
    QuadInputOutputFastRead4Byte = 0xEC,   // 4-BYTE Quad Input/Output Fast Read Command, 1-4-4 4-4-4
    DtrQuadInputOutputFastRead4Byte = 0xEE,   // 4-BYTE Quad Input/Output Fast Read Command DTR, 4-4-4
    // Erase category commands
    EraseSector = 0xD8, // Erase sector 1-1-0 2-2-0 4-4-0
    EraseSubsector4KB = 0x20, // Erase 4KB subsector 1-1-0 2-2-0 4-4-0
    EraseSubsector32KB = 0x52, // Erase 32KB subsector 1-1-0 2-2-0 4-4-0
    EraseDie = 0xC4, // Erase whole die 1-1-0 2-2-0 4-4-0
    EraseSector4Byte = 0xDC, // 4-BYTE Erase sector 1-1-0 2-2-0 4-4-0
    EraseSubsector4KB4Byte = 0x21, // 4-BYTE Erase 4KB subsector 1-1-0 2-2-0 4-4-0
    EraseSubsector32KB4Byte = 0x5C, // 4-BYTE Erase 32KB subsector 1-1-0 2-2-0 4-4-0
    // Program commands
    ProgramPage = 0x02, // PAGE PROGRAM 1-1-1 2-2-2 4-4-4
    QuadInputFastProgram = 0x32, // QUAD INPUT FAST PROGRAM 1-1-4 4-4-4
    ExtendedQuadInputFastProgram = 0x38, // EXTENDED QUAD INPUT FAST PROGRAM 1-4-4 4-4-4
    ProgramPage4Byte = 0x12, // 4-BYTE PAGE PROGRAM 1-1-1 2-2-2 4-4-4
    QuadInputFastProgram4Byte = 0x34, // 4-BYTE QUAD INPUT FAST PROGRAM 1-1-4 4-4-4
    ExtendedQuadInputFastProgram4Byte = 0x3E // 4-BYTE EXTENDED QUAD INPUT FAST PROGRAM 1-4-4 4-4-4
};
constexpr bool is_valid_flash_cmd(FlashCmd cmd) {
    // magic_enum automatically scans the enum and checks if 'cmd' matches a valid identifier
    return magic_enum::enum_contains(cmd);
}

enum class BusProtocol : uint8_t { Extended_SPI, Dual_SPI, Quad_SPI };
constexpr bool is_valid_bus_protocol(BusProtocol prot) {
    return magic_enum::enum_contains(prot);
}

enum class TransferRate : uint8_t { DTR, STR };
constexpr bool is_valid_transfer_rate(TransferRate rate) {
    return magic_enum::enum_contains(rate);
}

enum class AddressBytes : uint8_t { ADDR_LEN_ANY, ADDR_LEN_3 = 3, ADDR_LEN_4 = 4 };
constexpr bool is_valid_requested_address_bytes(AddressBytes addr_bytes) {
    return magic_enum::enum_contains(addr_bytes);
}

// C++20 Concept to validate that a policy type matches the Flash specifications
template <typename T>
concept ValidFlashDevice = requires {
    // Enforce that T contains a static member called 'profile'
    { T::profile } -> std::convertible_to<const FlashDeviceProfile&>;
};

// Define specific vendor hardware variations as compile-time constants
struct MT25QU02GCBB {
    static inline FlashDeviceProfile profile = {
        .model_name = "Micron_MT25QU02GCBB",
        .jedec_id = {0x20, 0xBB, 0x22}, // Matches your required 0x20, 0xBB, 0x22 values
        // from tn2506_sfdp_for_mt25q.pdf
        .sfdp = {
        /* 00h */        0x53, 0x46, 0x44, 0x50, 0x06, 0x01, 0x01, 0xFF, 0x00, 0x06, 0x01, 0x10, 0x30, 0x00, 0x00, 0xFF,
        /* 10h */        0x84, 0x00, 0x01, 0x02, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        /* 20h */        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        /* 30h */        0xE5, 0x20, 0xFB, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x29, 0xEB, 0x27, 0x6B, 0x27, 0x3B, 0x27, 0xBB,
        /* 40h */        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x27, 0xBB, 0xFF, 0xFF, 0x29, 0xEB, 0x0C, 0x20, 0x10, 0xD8,
        /* 50h */        0x0F, 0x52, 0x00, 0x00, 0x24, 0x4A, 0x99, 0x00, 0x8B, 0x8E, 0x03, 0xE1, 0xAC, 0x01, 0x27, 0x38,
        /* 60h */        0x7A, 0x75, 0x7A, 0x75, 0xFB, 0xBD, 0xD5, 0x5C, 0x4A, 0x0F, 0x82, 0xFF, 0x81, 0xBD, 0x3D, 0x36,
        /* 70h */        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        /* 80h */        0xFF, 0xE7, 0xFF, 0xFF, 0x21, 0xDC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		/* 90h */        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		/* A0h */        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		/* B0h */        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		/* C0h */        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		/* D0h */        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		/* E0h */        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		/* F0h */        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
        },   
        .sector_num = 4096,
        .sector_size = 65536,
        .subsector_32KB_num = 8192,
        .subsector_4KB_num = 65536,
        .die_num = 8,
        .config = {
            .dummy_cycles = 15, // 1111 = default dummy for each read command
            .xip_mode = 7, // 111 = disable (default)
            .driver_strength = 7, // 111 = 30ohms (default)
            .transfer_rate = 1, // 0 = enable (DTR), 1 = disable (STR default)
            .hold = 1, // 0 = enabled, 1 = disabled (default)
            .quad_io = 1, // 0 = enabled, 1 = disabled (default)
            .dual_io = 1, // 0 = enabled, 1 = disabled (default)
            .select_128Mb = 1, // 0 = highest 128Mb segment, 1 = lowest 128Mb segment (default)
            .addr_len = 1 // 0 = 4-byte address, 1 = 3-byte address (default)
        }
    };

    static uint32_t subsector_32KB_size() noexcept {
        return profile.sector_size / 2;
    }

    static uint32_t subsector_4KB_size() noexcept {
        return profile.sector_size / 16;
    }

    static uint32_t die_size() noexcept {
        return profile.sector_size * profile.sector_num;
    }

    static AddressBytes get_addr_len() noexcept {
        return profile.config.addr_len == 0 ? AddressBytes::ADDR_LEN_4 : AddressBytes::ADDR_LEN_3;
    } 

    static BusProtocol get_bus_protocol() noexcept {
        if (profile.config.dual_io == 1 && profile.config.quad_io == 1) {
            return BusProtocol::Extended_SPI;
        } else if (profile.config.dual_io == 0 && profile.config.quad_io == 1) {
            return BusProtocol::Dual_SPI;
        } else if (profile.config.dual_io == 1 && profile.config.quad_io == 0) {
            return BusProtocol::Quad_SPI;
        } else {
            // Invalid configuration where both dual and quad are disabled, default to Extended_SPI
            return BusProtocol::Extended_SPI;
        }
    }

    static TransferRate get_transfer_rate() noexcept {
        return profile.config.transfer_rate == 0 ? TransferRate::DTR : TransferRate::STR;
    }
};

// Enforce valid Micron Flash address widths (3 bytes or 4 bytes only)
template <size_t Width>
concept ValidAddressMode = (Width == 3) || (Width == 4);

// Enforce valid dummy clock ranges based on Micron hardware restrictions
template <uint8_t Cycles>
concept ValidDummyCycles = (Cycles >= 0) && (Cycles <= 14);

// Coomands traits structure and compile-time command matrix
// ---------------------------------------------------------
struct CommandTraits {
    FlashCmd cmd;
    BusProtocol protocol;
    AddressBytes requested_address_bytes; // 0 for all, 4 for 4-byte addresses
    uint8_t force_dtr; // native DTR command regardless config.transfer_rate
    uint8_t dummy_clocks_str;
    uint8_t dummy_clocks_dtr;

    // You must explicitly provide a default constructor so the std::array can initialize empty slots
    constexpr CommandTraits() : cmd(static_cast<FlashCmd>(0)), protocol(BusProtocol::Extended_SPI), 
        requested_address_bytes(AddressBytes::ADDR_LEN_ANY), force_dtr(0), dummy_clocks_str(0), dummy_clocks_dtr(0) {}

    // A Templated Factory Function that intercepts variables and forces concept validation
    template <uint8_t DummyClocksStr, uint8_t DummyClocksDtr>
    requires (ValidDummyCycles<DummyClocksStr> && ValidDummyCycles<DummyClocksDtr>)
    static constexpr CommandTraits create(FlashCmd c, BusProtocol p = BusProtocol::Extended_SPI, 
        AddressBytes a = AddressBytes::ADDR_LEN_ANY, bool force_dtr = false) {
        if (!is_valid_flash_cmd(c)) {
            throw "Error: Attempted to create CommandTraits with an unassigned raw command ID!";
        }
        
        if (!is_valid_bus_protocol(p)) {
            throw "Error: Attempted to create CommandTraits with an invalid bus protocol!";
        }

        if (!is_valid_requested_address_bytes(a)) {
            throw "Error: Attempted to create CommandTraits with an invalid requested address byte value!";
        }

        return CommandTraits{c, p, a, force_dtr, DummyClocksStr, DummyClocksDtr};
    } 

    template <uint8_t DummyClocksStr, uint8_t DummyClocksDtr>
    static constexpr CommandTraits create(FlashCmd c, AddressBytes a) {
        return create<DummyClocksStr, DummyClocksDtr>(c, BusProtocol::Extended_SPI, a, false);
    }

    template <uint8_t DummyClocksStr, uint8_t DummyClocksDtr>
    static constexpr CommandTraits create(FlashCmd c, bool force_dtr) {
        return create<DummyClocksStr, DummyClocksDtr>(c, BusProtocol::Extended_SPI, AddressBytes::ADDR_LEN_ANY, force_dtr);
    }

    template <uint8_t DummyClocksStr, uint8_t DummyClocksDtr>
    static constexpr CommandTraits create(FlashCmd c, AddressBytes a, bool force_dtr) {
        return create<DummyClocksStr, DummyClocksDtr>(c, BusProtocol::Extended_SPI, a, force_dtr);
    }

private:
    // Private backing constructor to prevent un-validated structures
    constexpr CommandTraits(FlashCmd c, BusProtocol p, AddressBytes a, bool f_dtr, uint8_t d_str, uint8_t d_dtr)
        : cmd(c), protocol(p), requested_address_bytes(a), force_dtr(f_dtr ? 1 : 0), dummy_clocks_str(d_str), dummy_clocks_dtr(d_dtr) {}
};

// A flat array containing exactly 256 command entries * 4 BusProtocols = 1024 total entries, 
// but we will only populate valid ones. This allows for O(1) lookup of command traits at runtime 
// without any hashing or branching, as the command byte directly maps to an index in this array. 
// The bus protocol is encoded in the upper bits of the index to allow for multiple protocols with the same command code.
// Lives in contiguous memory, meaning perfect CPU L1-Cache utilization
inline constexpr std::array<CommandTraits, 256 * 4> CommandMatrix = []() {
    // 4 bus protocols * 256 possible command codes = 1024 total entries, but we will only populate valid ones
    std::array<CommandTraits, 256 * 4> table{}; 
    // Define a local helper lambda right here!
    // It captures nothing, takes an enum, and converts it to its raw index size_t.
    auto idx = [](FlashCmd cmd, BusProtocol prot = BusProtocol::Extended_SPI) constexpr { 
        uint32_t p = static_cast<uint32_t>(prot) << 8;
        uint32_t c = static_cast<uint32_t>(cmd);
        return static_cast<size_t>(p | c); 
    };

    // Initialize array at compile-time...
    table[idx(FlashCmd::ResetEnable) ] = CommandTraits::create<0, 0>(FlashCmd::ResetEnable);
    table[idx(FlashCmd::ResetMemory) ] = CommandTraits::create<0, 0>(FlashCmd::ResetMemory);
    table[idx(FlashCmd::ReadId     ) ] = CommandTraits::create<0, 0>(FlashCmd::ReadId     );
    table[idx(FlashCmd::ReadSFDP   ) ] = CommandTraits::create<8, 8>(FlashCmd::ReadSFDP   );
    table[idx(FlashCmd::WriteEnable) ] = CommandTraits::create<0, 0>(FlashCmd::WriteEnable);
    table[idx(FlashCmd::WriteDisable)] = CommandTraits::create<0, 0>(FlashCmd::WriteDisable);
    table[idx(FlashCmd::Enter4Byte ) ] = CommandTraits::create<0, 0>(FlashCmd::Enter4Byte );
    table[idx(FlashCmd::Exit4Byte  ) ] = CommandTraits::create<0, 0>(FlashCmd::Exit4Byte  );
    // Read category commands
    table[idx(FlashCmd::Read       ) ] = CommandTraits::create<0, 0>(FlashCmd::Read       );
    table[idx(FlashCmd::FastRead   ) ] = CommandTraits::create<8, 6>(FlashCmd::FastRead   );
    table[idx(FlashCmd::DtrFastRead) ] = CommandTraits::create<0, 6>(FlashCmd::DtrFastRead, true);    
    table[idx(FlashCmd::QuadOutputFastRead)]    = CommandTraits::create<8, 6>(FlashCmd::QuadOutputFastRead);
    table[idx(FlashCmd::DtrQuadOutputFastRead)] = CommandTraits::create<0, 6>(FlashCmd::DtrQuadOutputFastRead, true);
    table[idx(FlashCmd::QuadInputOutputFastRead)]    = CommandTraits::create<10, 8>(FlashCmd::QuadInputOutputFastRead);      
    table[idx(FlashCmd::DtrQuadInputOutputFastRead)] = CommandTraits::create<0, 8>(FlashCmd::DtrQuadInputOutputFastRead, true);      
    table[idx(FlashCmd::Read4Byte)]     = CommandTraits::create<0, 0>(FlashCmd::Read4Byte, AddressBytes::ADDR_LEN_4);
    table[idx(FlashCmd::FastRead4Byte)]    = CommandTraits::create<8, 6>(FlashCmd::FastRead4Byte, AddressBytes::ADDR_LEN_4);    
    table[idx(FlashCmd::DtrFastRead4Byte)] = CommandTraits::create<0, 6>(FlashCmd::DtrFastRead4Byte, AddressBytes::ADDR_LEN_4, true);      
    table[idx(FlashCmd::QuadOutputFastRead4Byte)] = CommandTraits::create<8, 6>(FlashCmd::QuadOutputFastRead4Byte, AddressBytes::ADDR_LEN_4);    
    table[idx(FlashCmd::QuadInputOutputFastRead4Byte)] = CommandTraits::create<10, 8>(FlashCmd::QuadInputOutputFastRead4Byte, AddressBytes::ADDR_LEN_4);    
    table[idx(FlashCmd::DtrQuadInputOutputFastRead4Byte)] = CommandTraits::create<0, 8>(FlashCmd::DtrQuadInputOutputFastRead4Byte, AddressBytes::ADDR_LEN_4, true);   
    // Erase category commands
    table[idx(FlashCmd::EraseSector)] = CommandTraits::create<0, 0>(FlashCmd::EraseSector); 
    table[idx(FlashCmd::EraseSubsector4KB)] = CommandTraits::create<0, 0>(FlashCmd::EraseSubsector4KB); 
    table[idx(FlashCmd::EraseSubsector32KB)] = CommandTraits::create<0, 0>(FlashCmd::EraseSubsector32KB); 
    table[idx(FlashCmd::EraseDie)] = CommandTraits::create<0, 0>(FlashCmd::EraseDie); 
    table[idx(FlashCmd::EraseSector4Byte)] = CommandTraits::create<0, 0>(FlashCmd::EraseSector4Byte, AddressBytes::ADDR_LEN_4); 
    table[idx(FlashCmd::EraseSubsector4KB4Byte)] = CommandTraits::create<0, 0>(FlashCmd::EraseSubsector4KB4Byte, AddressBytes::ADDR_LEN_4); 
    table[idx(FlashCmd::EraseSubsector32KB4Byte)] = CommandTraits::create<0, 0>(FlashCmd::EraseSubsector32KB4Byte, AddressBytes::ADDR_LEN_4); 
    // Program category command
    table[idx(FlashCmd::ProgramPage)] = CommandTraits::create<0, 0>(FlashCmd::ProgramPage); 
    table[idx(FlashCmd::QuadInputFastProgram)] = CommandTraits::create<0, 0>(FlashCmd::QuadInputFastProgram); 
    table[idx(FlashCmd::ExtendedQuadInputFastProgram)] = CommandTraits::create<0, 0>(FlashCmd::ExtendedQuadInputFastProgram); 
    table[idx(FlashCmd::ProgramPage4Byte)] = CommandTraits::create<0, 0>(FlashCmd::ProgramPage4Byte, AddressBytes::ADDR_LEN_4); 
    table[idx(FlashCmd::QuadInputFastProgram4Byte)] = CommandTraits::create<0, 0>(FlashCmd::QuadInputFastProgram4Byte, AddressBytes::ADDR_LEN_4); 
    table[idx(FlashCmd::ExtendedQuadInputFastProgram4Byte)] = CommandTraits::create<0, 0>(FlashCmd::ExtendedQuadInputFastProgram4Byte, AddressBytes::ADDR_LEN_4); 
    return table;
}();


// Pure O(1) runtime lookup tool for your SystemC b_transport method
template <typename T>
inline std::optional<CommandTraits> get_traits(FlashCmd cmd) noexcept {
    uint32_t p = static_cast<uint32_t>(T::get_bus_protocol()) << 8;
    uint32_t c = static_cast<uint32_t>(cmd);
    auto idx =  static_cast<size_t>(p | c);     
    
    if (idx >= CommandMatrix.size()) [[unlikely]] {
        return std::nullopt; // Out-of-bounds command code, immediately return no traits
    }

    const auto& traits = CommandMatrix[idx];
    if (traits.cmd == cmd && traits.protocol == T::get_bus_protocol()) [[likely]] {
        return traits; // Valid command found, return its traits
    } else {
        return std::nullopt; // No valid command traits found for this target
    }
}

// The main FlashModel SystemC module definition
// ---------------------------------------------
template <typename DevicePolicy> requires ValidFlashDevice<DevicePolicy>
class FlashModel : public sc_core::sc_module {
public:
    // TLM 2.0 target socket initialization
    tlm_utils::simple_target_socket<FlashModel<DevicePolicy>, 32> flash_socket;

    SC_HAS_PROCESS(FlashModel);
    
    explicit FlashModel(sc_core::sc_module_name name) 
        : sc_module(name), flash_socket("flash_socket"), flash_storage("flash_storage") 
    {
        flash_storage.open_storage("flash_backing_storage.bin", get_capacity());

        // Bind the transaction function using C++11/C++14 style lambdas cleanly
        flash_socket.register_b_transport(this, &FlashModel<DevicePolicy>::b_transport);
    }

    size_t get_capacity() const noexcept {
        size_t die_size = DevicePolicy::profile.sector_size * DevicePolicy::profile.sector_num;
        return DevicePolicy::profile.die_num * die_size;
    }

    size_t get_erase_size(const FlashCmd cmd) const noexcept {
        size_t size = 0;
    
        switch (cmd)
        {
        case FlashCmd::EraseSubsector32KB:
        case FlashCmd::EraseSubsector32KB4Byte:
            size = DevicePolicy::subsector_32KB_size();
            break;
        case FlashCmd::EraseSubsector4KB:
        case FlashCmd::EraseSubsector4KB4Byte:
            size = DevicePolicy::subsector_4KB_size();
            break;
        case FlashCmd::EraseSector:
        case FlashCmd::EraseSector4Byte:
            size = DevicePolicy::profile.sector_size;
            break;
        case FlashCmd::EraseDie:
            size = DevicePolicy::die_size();
            break;
        default:
            break;
        }
    
        return size;
    }

    std::string get_back_file_name() const noexcept {
        return flash_storage.get_back_file_name();
    }

private:
    // utility
    uint32_t get_addr(uint8_t* stream, uint32_t addr_len) const noexcept {
        uint32_t addr = 0;
        switch (addr_len) {
            case 3:
                addr = ((uint32_t)stream[0] << 16) | 
                       ((uint32_t)stream[1] << 8) | stream[2];
                break;
            case 4:
                addr = ((uint32_t)stream[0] << 24) | 
                       ((uint32_t)stream[1] << 16) | 
                       ((uint32_t)stream[2] << 8) | stream[3];
                break;
            default:
                SC_REPORT_ERROR("FlashModel", make_msg("Unsupported address length %u.", addr_len).c_str());
                return 0;
        }
        return addr;
    }

    // Internal registers matching the hardware specification state machines
    bool m_reset_enabled{false};
    bool m_write_enabled{false}; // New internal state to track write enable status
    FlashStorage flash_storage; // Encapsulates the memory-mapped file storage logic

    // Core TLM blocking transport implementation method
    void b_transport(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) noexcept;

    // Command controller execution unit
    int process_flash_cmd(CommandTraits& traits, uint8_t* stream, unsigned int len) noexcept;

    int read_id(uint8_t* stream, unsigned int len) noexcept;
    int read_sfdp(uint8_t* stream, unsigned int len, uint32_t dummy_clocks) noexcept;
    int read_flash(const CommandTraits& traits, uint8_t* stream, size_t len) noexcept;
    int erase_flash(const CommandTraits& traits, uint8_t* stream, size_t len) noexcept;
    int program_flash(const CommandTraits& traits, uint8_t* stream, size_t len) noexcept;
};

