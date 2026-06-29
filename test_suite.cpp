#define CATCH_CONFIG_RUNNER  
#include <catch2/catch.hpp>

#include <memory.h>
#include "flash.h"
#include "testbench.h"

#include <fstream>
#include <array>
#include <span>

// Declare global unique_ptrs so they are accessible to Catch2 test cases
// but live safely managed across the SystemC lifetime loop
inline FlashModel<MT25QU02GCBB>* g_flash_device = nullptr;
inline Testbench*  g_tb = nullptr;

// Helper to easily inject golden mock data into the backing file from a test case
static void plant_flash_data(const std::string& filepath, size_t offset, std::span<const uint8_t> data) {
    // Open the existing storage file in binary, input/output modification mode
    std::ofstream fs(filepath, std::ios::in | std::ios::out | std::ios::binary);
    if (!fs.is_open()) {
        FAIL("Failed to open the flash storage file to plant mock test data.");
    }
    
    // Seek to the designated physical flash address offset
    fs.seekp(offset, std::ios::beg);
    fs.write(reinterpret_cast<const char*>(data.data()), data.size());
    fs.close();
}

static void fetch_flash_data(const std::string& filepath, size_t offset, std::span<uint8_t> data) {
    // Open the existing storage file in binary, input/output modification mode
    std::ifstream fs(filepath, std::ios::in | std::ios::binary);
    if (!fs.is_open()) {
        FAIL("Failed to open the flash storage file to plant mock test data.");
    }
    
    // Seek to the designated physical flash address offset
    fs.seekg(offset, std::ios::beg);
    fs.read(reinterpret_cast<char*>(data.data()), data.size());
    fs.close();
}

static void write_enable() {
    std::array<uint8_t, 1> spi_stream;

    spi_stream[0] = static_cast<uint8_t>(FlashCmd::WriteEnable); // Opcode
    auto status = g_tb->exchange_stream(spi_stream.data(), spi_stream.size());
    REQUIRE(status == tlm::TLM_OK_RESPONSE);
}

static void write_disable() {
    std::array<uint8_t, 1> spi_stream;
    
    spi_stream[0] = static_cast<uint8_t>(FlashCmd::WriteDisable); // Opcode
    auto status = g_tb->exchange_stream(spi_stream.data(), spi_stream.size());
    REQUIRE(status == tlm::TLM_OK_RESPONSE);
}

static void read_register(FlashCmd cmd, FlashDeviceConfig& value) {
    std::array<uint8_t, 3> spi_stream = {};
    
    spi_stream[0] = static_cast<uint8_t>(cmd); // Opcode
    auto status = g_tb->exchange_stream(spi_stream.data(), spi_stream.size());
    REQUIRE(status == tlm::TLM_OK_RESPONSE);

    uint16_t raw = (static_cast<uint16_t>(spi_stream[1]) << 8) | spi_stream[2];
    value = std::bit_cast<FlashDeviceConfig>(raw);
}

static void write_register(FlashCmd cmd, FlashDeviceConfig value) {
    write_enable();

    std::array<uint8_t, 3> spi_stream = {};
    uint16_t raw = std::bit_cast<uint16_t>(value);
    
    spi_stream[0] = static_cast<uint8_t>(cmd); // Opcode
    spi_stream[1] = raw >> 8;
    spi_stream[2] = raw & 0xFF;
    auto status = g_tb->exchange_stream(spi_stream.data(), spi_stream.size());
    REQUIRE(status == tlm::TLM_OK_RESPONSE);

    write_disable();
}

static void enter_4bytes() {
    write_enable();

    std::array<uint8_t, 1> spi_stream = {};
    spi_stream[0] = static_cast<uint8_t>(FlashCmd::Enter4Byte); // Opcode
    int status = g_tb->exchange_stream(spi_stream.data(), spi_stream.size());
    REQUIRE(status == tlm::TLM_OK_RESPONSE);

    write_disable();
}

static void exit_4bytes() {
    write_enable();

    std::array<uint8_t, 1> spi_stream = {};
    spi_stream[0] = static_cast<uint8_t>(FlashCmd::Exit4Byte); // Opcode
    int status = g_tb->exchange_stream(spi_stream.data(), spi_stream.size());
    REQUIRE(status == tlm::TLM_OK_RESPONSE);

    write_disable();
}

static void set_dual_spi() {
    FlashDeviceConfig config;
    read_register(FlashCmd::ReadVolatileConfigure, config);
    config.dual_io = 0; // 0: enable, 1: disable (default)
    write_register(FlashCmd::WriteVolatileConfigure, config);
    config = {};
    read_register(FlashCmd::ReadVolatileConfigure, config);
    REQUIRE(config.dual_io == 0);
}

static void set_quad_spi() {
    FlashDeviceConfig config;
    read_register(FlashCmd::ReadVolatileConfigure, config);
    config.quad_io = 0; // 0: enable, 1: disable (default)
    write_register(FlashCmd::WriteVolatileConfigure, config);
    config = {};
    read_register(FlashCmd::ReadVolatileConfigure, config);
    REQUIRE(config.quad_io == 0);
}

template <uint8_t AddrBytes, uint8_t DummyClocks>
requires (ValidAddressMode<AddrBytes> && ValidSingleDummyCycle<DummyClocks>)
void read_flash_test(const FlashCmd cmd) noexcept {
    REQUIRE(g_tb != nullptr); // Sanity check to ensure the global testbench pointer is valid
    REQUIRE(g_flash_device != nullptr); // Sanity check to ensure the global flash model pointer is valid
    REQUIRE(is_valid_flash_cmd(cmd)); // Ensure the command and protocol are valid

    uint32_t data_len = std::rand() % 256 + 1; // Random data length between 1 and 256 bytes
    size_t addr = std::rand() % (g_flash_device->get_capacity() - data_len); // Random address within bounds
    if (AddrBytes == 3) addr %= (0x1000000); // Ensure it fits within 3 bytes
    std::vector<uint8_t> golden_data(data_len);
    std::iota(golden_data.begin(), golden_data.end(), std::rand() % 256); // Fill with random values

    // Insert test data into flash backing file at the correct offset so the model can read it back during the test
    plant_flash_data(g_flash_device->get_back_file_name(), addr, golden_data);

    // 1. Calculate overall stream size: 
    // 1 byte (Opcode) + AddressBytes + 1 byte (Dummy Clock + Bus protocol) + Variable Response Data Size
    uint32_t hdr_len = 1 + AddrBytes + 1; // Header length before the data payload
    size_t total_stream_size = hdr_len + data_len;
    std::vector<uint8_t> cmd_stream(total_stream_size, 0x00);

    // 2. Compose the structural SPI Command Stream protocol header
    uint32_t stream_idx = 0;
    cmd_stream[stream_idx++] = static_cast<uint8_t>(cmd);
    if (AddrBytes == 4) {
        cmd_stream[stream_idx++] = static_cast<uint8_t>((addr >> 24) & 0xFF); // Address Byte 3 (MSB)
    } 
    cmd_stream[stream_idx++] = static_cast<uint8_t>((addr >> 16) & 0xFF); // Address Byte 2 
    cmd_stream[stream_idx++] = static_cast<uint8_t>((addr >> 8) & 0xFF);  // Address Byte 1
    cmd_stream[stream_idx++] = static_cast<uint8_t>(addr & 0xFF);         // Address Byte 0 (LSB)
    cmd_stream[stream_idx++] = static_cast<uint8_t>(DummyClocks & 0xFF); // Bus protocol and dummy clock byte cycle overhead
        
    // 3. Fire the execution stream over your SystemC simulation interface
    auto status = g_tb->exchange_stream(cmd_stream.data(), cmd_stream.size());
    
    // 4. Assertions
    REQUIRE(status == tlm::TLM_OK_RESPONSE);

    // FIXED: Isolate the readback payload window using a C++20 view to match against golden_data
    auto readback_window = std::span<const uint8_t>(cmd_stream.data() + hdr_len, data_len);
    // Explicitly forces an element-by-element deep comparison 
    REQUIRE(std::equal(readback_window.begin(), readback_window.end(), golden_data.begin(), golden_data.end()));
}

template <uint8_t AddrBytes> requires (ValidAddressMode<AddrBytes>)
void erase_flash_test(const FlashCmd cmd) noexcept {
    REQUIRE(g_tb != nullptr); // Sanity check to ensure the global testbench pointer is valid
    REQUIRE(g_flash_device != nullptr); // Sanity check to ensure the global flash model pointer is valid
    REQUIRE(is_valid_flash_cmd(cmd)); // Ensure the command and protocol are valid

    write_enable();

    uint32_t addr = std::rand() % (g_flash_device->get_capacity()); // Random address within bounds
    if (AddrBytes == 3) addr %= (0x1000000); // Ensure it fits within 3 bytes
    size_t size = g_flash_device->get_erase_size(cmd);
    size_t start = (addr & ~(size - 1)); // mask to the beginning of erase unit

    // 1. Calculate overall stream size: 
    // 1 byte (Opcode) + AddressBytes + 1 byte (Dummy Clock + Bus protocol) + Variable Response Data Size
    uint32_t hdr_len = 1 + AddrBytes + 1; // Header length before the data payload
    std::vector<uint8_t> cmd_stream(hdr_len, 0x00);

    // 2. Compose the structural SPI Command Stream protocol header
    uint32_t stream_idx = 0;
    cmd_stream[stream_idx++] = static_cast<uint8_t>(cmd);
    if (AddrBytes == 4) {
        cmd_stream[stream_idx++] = static_cast<uint8_t>((addr >> 24) & 0xFF); // Address Byte 3 (MSB)
    } 
    cmd_stream[stream_idx++] = static_cast<uint8_t>((addr >> 16) & 0xFF); // Address Byte 2 
    cmd_stream[stream_idx++] = static_cast<uint8_t>((addr >> 8) & 0xFF);  // Address Byte 1
    cmd_stream[stream_idx++] = static_cast<uint8_t>(addr & 0xFF);         // Address Byte 0 (LSB)
        
    // 3. Fire the execution stream over your SystemC simulation interface
    auto status = g_tb->exchange_stream(cmd_stream.data(), cmd_stream.size());
    
    // 4. Assertions
    REQUIRE(status == tlm::TLM_OK_RESPONSE);

    // Insert test data into flash backing file at the correct offset so the model can read it back during the test
    std::vector<uint8_t> golden_data(size, 0xFF);
    std::vector<uint8_t> erased_data(size);
    fetch_flash_data(g_flash_device->get_back_file_name(), start, erased_data);
    // Explicitly forces an element-by-element deep comparison 
    REQUIRE(std::equal(erased_data.begin(), erased_data.end(), golden_data.begin(), golden_data.end()));

    write_disable();
}

template <uint8_t AddrBytes> requires (ValidAddressMode<AddrBytes>)
void program_flash_test(const FlashCmd cmd) noexcept {
    REQUIRE(g_tb != nullptr); // Sanity check to ensure the global testbench pointer is valid
    REQUIRE(g_flash_device != nullptr); // Sanity check to ensure the global flash model pointer is valid
    REQUIRE(is_valid_flash_cmd(cmd)); // Ensure the command and protocol are valid

    write_enable();

    uint32_t data_len = std::rand() % 256 + 1; // Random data length between 1 and 256 bytes
    size_t addr = std::rand() % (g_flash_device->get_capacity() - data_len); // Random address within bounds
    if (AddrBytes == 3) addr %= (0x1000000); // Ensure it fits within 3 bytes
    std::vector<uint8_t> golden_data(data_len);
    std::iota(golden_data.begin(), golden_data.end(), std::rand() % 256); // Fill with random values

    // Calculate overall stream size: 
    uint32_t hdr_len = 1 + AddrBytes; // Header length before the data payload
    std::vector<uint8_t> cmd_stream(hdr_len + data_len);

    // Compose the structural SPI Command Stream protocol header
    uint32_t stream_idx = 0;
    cmd_stream[stream_idx++] = static_cast<uint8_t>(cmd);
    if (AddrBytes == 4) {
        cmd_stream[stream_idx++] = static_cast<uint8_t>((addr >> 24) & 0xFF); // Address Byte 3 (MSB)
    } 
    cmd_stream[stream_idx++] = static_cast<uint8_t>((addr >> 16) & 0xFF); // Address Byte 2 
    cmd_stream[stream_idx++] = static_cast<uint8_t>((addr >> 8) & 0xFF);  // Address Byte 1
    cmd_stream[stream_idx++] = static_cast<uint8_t>(addr & 0xFF);         // Address Byte 0 (LSB)
    
    // Fill program data
    std::copy_n(golden_data.data(), golden_data.size(), &cmd_stream[stream_idx]);

    // Fire the execution stream over your SystemC simulation interface
    auto status = g_tb->exchange_stream(cmd_stream.data(), cmd_stream.size());
    
    // Assertions
    REQUIRE(status == tlm::TLM_OK_RESPONSE);

    // Verify programed data in flash back file
    std::vector<uint8_t> program_data(data_len, 0x00);
    fetch_flash_data(g_flash_device->get_back_file_name(), addr, program_data);
    // Explicitly forces an element-by-element deep comparison 
    REQUIRE(std::equal(program_data.begin(), program_data.end(), golden_data.begin(), golden_data.end()));

    write_disable();
}

TEST_CASE("Micron Flash Model Protocol Stream Automated Tests", "[flash]") {
    SECTION("1. READ_ID Bi-directional Stream Check") {
        REQUIRE(g_tb != nullptr); // Sanity check to ensure the global testbench pointer is valid
        REQUIRE(g_flash_device != nullptr); // Sanity check to ensure the global flash model pointer is valid

        // 1 byte opcode + 3 bytes space allocated for the response
        std::array<uint8_t, 4> spi_stream = { static_cast<uint8_t>(FlashCmd::ReadId), 0, 0, 0 };

        // Use the global pointer securely
        auto status = g_tb->exchange_stream(spi_stream.data(), spi_stream.size());

        REQUIRE(status == tlm::TLM_OK_RESPONSE);
        REQUIRE(spi_stream[0] == 0x20); // Verifies Manufacturer ID overwritten by target
        REQUIRE(spi_stream[1] == 0xBB); 
        REQUIRE(spi_stream[2] == 0x22); // Verifies Capacity ID overwritten by target
    }

    SECTION("2. READ_SFDP Bi-directional Stream Check") {
        std::array<uint8_t, 256> spi_stream = { 
            static_cast<uint8_t>(FlashCmd::ReadSFDP), // Opcode
            0x00, 0x00, 0x00, // 3 bytes Address
            0x08        // 1 byte Dummy_cycles
        };

        std::array<uint8_t, 4> sfdp_signature = {
            'S', 'F', 'D', 'P'
        };

        auto status = g_tb->exchange_stream(spi_stream.data(), spi_stream.size());
        bool sfdp_matches = std::equal(
            spi_stream.begin() + 5, spi_stream.begin() + 5 + sfdp_signature.size(), // Isolate the readback window indices
            sfdp_signature.begin(), sfdp_signature.end()
        );
        REQUIRE(status == tlm::TLM_OK_RESPONSE);
        REQUIRE(sfdp_matches);
    }

    SECTION("3. READ with 3-byte address 1-1-1") {
        read_flash_test<3, 0>(FlashCmd::Read);
    }

    SECTION("4. FAST_READ with 3-byte address 1-1-1 STR") {
        read_flash_test<3, 8>(FlashCmd::FastRead);
    }

    SECTION("5. DTR_FAST_READ with 3-byte address 1-1-1 DTR") {
        read_flash_test<3, 6>(FlashCmd::DtrFastRead);
    }

    SECTION("6. QUAD_OUTPUT_FAST_READ with 3-byte address 1-1-4 STR") {
        read_flash_test<3, 8>(FlashCmd::QuadOutputFastRead);
    }

    SECTION("7. DTR_QUAD_OUTPUT_FAST_READ with 3-byte address 1-1-4 DTR") {
        read_flash_test<3, 6>(FlashCmd::DtrQuadOutputFastRead);
    }

    SECTION("8. QUAD_INPUT_OUTPUT_FAST_READ with 3-byte address 1-4-4 STR") {
        read_flash_test<3, 10>(FlashCmd::QuadInputOutputFastRead);
    }

    SECTION("9. DTR_QUAD_INPUT_OUTPUT_FAST_READ with 3-byte address 1-4-4 DTR") {
        read_flash_test<3, 8>(FlashCmd::DtrQuadInputOutputFastRead);
    }

    SECTION("4. FAST_READ with 3-byte address 1-1-1 STR") {
        read_flash_test<3, 8>(FlashCmd::FastRead);
    }

    SECTION("5. DTR_FAST_READ with 3-byte address 1-1-1 DTR") {
        read_flash_test<3, 6>(FlashCmd::DtrFastRead);
    }

    SECTION("6. QUAD_OUTPUT_FAST_READ with 3-byte address 1-1-4 STR") {
        read_flash_test<3, 8>(FlashCmd::QuadOutputFastRead);
    }

    SECTION("7. DTR_QUAD_OUTPUT_FAST_READ with 3-byte address 1-1-4 DTR") {
        read_flash_test<3, 6>(FlashCmd::DtrQuadOutputFastRead);
    }

    SECTION("8. QUAD_INPUT_OUTPUT_FAST_READ with 3-byte address 1-4-4 STR") {
        read_flash_test<3, 10>(FlashCmd::QuadInputOutputFastRead);
    }

    SECTION("9. DTR_QUAD_INPUT_OUTPUT_FAST_READ with 3-byte address 1-4-4 DTR") {
        read_flash_test<3, 8>(FlashCmd::DtrQuadInputOutputFastRead);
    }    

    SECTION("10. ERASE_SECTOR with 3-byte address 1-1-0") {
        erase_flash_test<3>(FlashCmd::EraseSector);
    }

    SECTION("11. ERASE_SUBSECTOR_32KB with 3-byte address 1-1-0") {
        erase_flash_test<3>(FlashCmd::EraseSubsector32KB);
    }

    SECTION("12. ERASE_SUBSECTOR_4KB with 3-byte address 1-1-0") {
        erase_flash_test<3>(FlashCmd::EraseSubsector4KB);
    }

    SECTION("13. ERASE_DIE with 3-byte address 1-1-0") {
        erase_flash_test<3>(FlashCmd::EraseDie);
    }

    SECTION("14. PROGRAM_PAGE with 3-byte address 1-1-1") {
        program_flash_test<3>(FlashCmd::ProgramPage);
    }

    SECTION("15. QUAD_INPUT_FAST_PROGRAM with 3-byte address 1-1-4") {
        program_flash_test<3>(FlashCmd::QuadInputFastProgram);
    }

    SECTION("16. EXTENDED_QUAD_INPUT_FAST_PROGRAM with 3-byte address 1-4-4") {
        program_flash_test<3>(FlashCmd::ExtendedQuadInputFastProgram);
    }

    SECTION("50. Enter 4-Byte Mode 1-0-0") {
        enter_4bytes();
    }

    SECTION("51. READ_4BYTE with 4-byte address 1-1-1") {
        read_flash_test<4, 0>(FlashCmd::Read4Byte);
    }

    SECTION("52. FAST_READ_4BYTE with 4-byte address 1-1-1 STR") {
        read_flash_test<4, 8>(FlashCmd::FastRead4Byte);
    }

    SECTION("53. DTR_FAST_READ_4BYTE with 4-byte address 1-1-1 DTR") {
        read_flash_test<4, 6>(FlashCmd::DtrFastRead4Byte);
    }

    SECTION("54. QUAD_OUTPUT_FAST_READ_4BYTE with 4-byte address 1-1-4 STR") {
        read_flash_test<4, 8>(FlashCmd::QuadOutputFastRead4Byte);
    }

    SECTION("55. QUAD_INPUT/OUTPUT_FAST_READ_4BYTE with 4-byte address 1-4-4 STR") {
        read_flash_test<4, 10>(FlashCmd::QuadInputOutputFastRead4Byte);
    }

    SECTION("56. DTR_QUAD_INPUT/OUTPUT_FAST_READ_4BYTE with 4-byte address 1-4-4 STR") {
        read_flash_test<4, 8>(FlashCmd::DtrQuadInputOutputFastRead4Byte);
    }

    SECTION("57. ERASE_SECTOR_4BYTE with 4-byte address 1-1-0") {
        erase_flash_test<4>(FlashCmd::EraseSector4Byte);
    }

    SECTION("58. ERASE_SUBSECTOR_32KB_4BYTE with 4-byte address 1-1-0") {
        erase_flash_test<4>(FlashCmd::EraseSubsector32KB4Byte);
    }

    SECTION("59. ERASE_SUBSECTOR_4KB_4BYTE with 4-byte address 1-1-0") {
        erase_flash_test<4>(FlashCmd::EraseSubsector4KB4Byte);
    }

    SECTION("60. PROGRAM_PAGE_4BYTE with 4-byte address 1-1-1") {
        program_flash_test<4>(FlashCmd::ProgramPage4Byte);
    }

    SECTION("61. QUAD_INPUT_FAST_PROGRAM_4BYTE with 4-byte address 1-1-4") {
        program_flash_test<4>(FlashCmd::QuadInputFastProgram4Byte);
    }

    SECTION("62. EXTENDED_QUAD_INPUT_FAST_PROGRAM_4BYTE with 4-byte address 1-4-4") {
        program_flash_test<4>(FlashCmd::ExtendedQuadInputFastProgram4Byte);
    } 
    
    // QSPI
    SECTION("100. RESET 1-0-0") {
        std::array<uint8_t, 1> spi_stream;

        spi_stream[0] = static_cast<uint8_t>(FlashCmd::ResetEnable); // Opcode
        auto status = g_tb->exchange_stream(spi_stream.data(), spi_stream.size());
        REQUIRE(status == tlm::TLM_OK_RESPONSE);

        spi_stream[0] = static_cast<uint8_t>(FlashCmd::ResetMemory); // Opcode
        status = g_tb->exchange_stream(spi_stream.data(), spi_stream.size());
        REQUIRE(status == tlm::TLM_OK_RESPONSE);
    } 

    SECTION("101. SET QSPI 1-0-1") {
        set_quad_spi();
    }

    SECTION("104. FAST_READ with 3-byte address 4-4-4 STR") {
        read_flash_test<3, 10>(FlashCmd::FastRead);
    }

    SECTION("105. DTR_FAST_READ with 3-byte address 4-4-4 DTR") {
        read_flash_test<3, 8>(FlashCmd::DtrFastRead);
    }

    SECTION("106. QUAD_OUTPUT_FAST_READ with 3-byte address 4-4-4 STR") {
        read_flash_test<3, 10>(FlashCmd::QuadOutputFastRead);
    }

    SECTION("107. DTR_QUAD_OUTPUT_FAST_READ with 3-byte address 4-4-4 DTR") {
        read_flash_test<3, 8>(FlashCmd::DtrQuadOutputFastRead);
    }

    SECTION("108. QUAD_INPUT_OUTPUT_FAST_READ with 3-byte address 4-4-4 STR") {
        read_flash_test<3, 10>(FlashCmd::QuadInputOutputFastRead);
    }

    SECTION("109. DTR_QUAD_INPUT_OUTPUT_FAST_READ with 3-byte address 4-4-4 DTR") {
        read_flash_test<3, 8>(FlashCmd::DtrQuadInputOutputFastRead);
    }

    SECTION("110. ERASE_SECTOR with 3-byte address 4-4-0") {
        erase_flash_test<3>(FlashCmd::EraseSector);
    }

    SECTION("111. ERASE_SUBSECTOR_32KB with 3-byte address 4-4-0") {
        erase_flash_test<3>(FlashCmd::EraseSubsector32KB);
    }

    SECTION("112. ERASE_SUBSECTOR_4KB with 3-byte address 4-4-0") {
        erase_flash_test<3>(FlashCmd::EraseSubsector4KB);
    }

    SECTION("113. ERASE_DIE with 3-byte address 4-4-0") {
        erase_flash_test<3>(FlashCmd::EraseDie);
    }

    SECTION("114. PROGRAM_PAGE with 3-byte address 4-4-4") {
        program_flash_test<3>(FlashCmd::ProgramPage);
    }

    SECTION("115. QUAD_INPUT_FAST_PROGRAM with 3-byte address 4-4-4") {
        program_flash_test<3>(FlashCmd::QuadInputFastProgram);
    }

    SECTION("116. EXTENDED_QUAD_INPUT_FAST_PROGRAM with 3-byte address 4-4-4") {
        program_flash_test<3>(FlashCmd::ExtendedQuadInputFastProgram);
    } 
    
    SECTION("150. Enter 4-Byte Mode 4-0-0") {
        enter_4bytes();
    }

    SECTION("151. READ_4BYTE with 4-byte address 4-4-4") {
        read_flash_test<4, 0>(FlashCmd::Read4Byte);
    }

    SECTION("152. FAST_READ_4BYTE with 4-byte address 4-4-4 STR") {
        read_flash_test<4, 10>(FlashCmd::FastRead4Byte);
    }

    SECTION("153. DTR_FAST_READ_4BYTE with 4-byte address 4-4-4 DTR") {
        read_flash_test<4, 8>(FlashCmd::DtrFastRead4Byte);
    }

    SECTION("154. QUAD_OUTPUT_FAST_READ_4BYTE with 4-byte address 4-4-4 STR") {
        read_flash_test<4, 10>(FlashCmd::QuadOutputFastRead4Byte);
    }

    SECTION("155. QUAD_INPUT/OUTPUT_FAST_READ_4BYTE with 4-byte address 4-4-4 STR") {
        read_flash_test<4, 10>(FlashCmd::QuadInputOutputFastRead4Byte);
    }

    SECTION("156. DTR_QUAD_INPUT/OUTPUT_FAST_READ_4BYTE with 4-byte address 4-4-4 STR") {
        read_flash_test<4, 8>(FlashCmd::DtrQuadInputOutputFastRead4Byte);
    }

    SECTION("157. ERASE_SECTOR_4BYTE with 4-byte address 4-4-0") {
        erase_flash_test<4>(FlashCmd::EraseSector4Byte);
    }

    SECTION("158. ERASE_SUBSECTOR_32KB_4BYTE with 4-byte address 4-4-0") {
        erase_flash_test<4>(FlashCmd::EraseSubsector32KB4Byte);
    }

    SECTION("159. ERASE_SUBSECTOR_4KB_4BYTE with 4-byte address 4-4-0") {
        erase_flash_test<4>(FlashCmd::EraseSubsector4KB4Byte);
    }

    SECTION("160. PROGRAM_PAGE_4BYTE with 4-byte address 4-4-4") {
        program_flash_test<4>(FlashCmd::ProgramPage4Byte);
    }

    SECTION("161. QUAD_INPUT_FAST_PROGRAM_4BYTE with 4-byte address 4-4-4") {
        program_flash_test<4>(FlashCmd::QuadInputFastProgram4Byte);
    }

    SECTION("162. EXTENDED_QUAD_INPUT_FAST_PROGRAM_4BYTE with 4-byte address 4-4-4") {
        program_flash_test<4>(FlashCmd::ExtendedQuadInputFastProgram4Byte);
    }     

    SECTION("190. EXIT 4 BYTES 4-0-0") {
        exit_4bytes();
    }

    // Dual SPI Tests
    // ===============

    SECTION("200. DUAL SPI MODE") {
        set_dual_spi();

        SECTION("201. DUAL_OUTPUT_FAST_READ with 3-byte address 1-1-2 STR") {
            read_flash_test<3, 8>(FlashCmd::DualOutputFastRead);
        }

        SECTION("202. DUAL_I/O_FAST_READ with 3-byte address 1-2-2 STR") {
            read_flash_test<3, 4>(FlashCmd::DualInputOutputFastRead);
        }

        SECTION("203. DUAL_OUTPUT_FAST_READ with 3-byte address 2-2-2 STR") {
            read_flash_test<3, 8>(FlashCmd::DualOutputFastRead);
        }

        SECTION("204. DUAL_I/O_FAST_READ with 3-byte address 2-2-2 STR") {
            read_flash_test<3, 4>(FlashCmd::DualInputOutputFastRead);
        }

        SECTION("205. DTR_DUAL_OUTPUT_FAST_READ with 3-byte address 1-1-2 DTR") {
            read_flash_test<3, 6>(FlashCmd::DtrDualOutputFastRead);
        }

        SECTION("206. DTR_DUAL_I/O_FAST_READ with 3-byte address 1-2-2 DTR") {
            read_flash_test<3, 4>(FlashCmd::DtrDualInputOutputFastRead);
        }

        SECTION("207. DTR_DUAL_OUTPUT_FAST_READ with 3-byte address 2-2-2 DTR") {
            read_flash_test<3, 6>(FlashCmd::DtrDualOutputFastRead);
        }

        SECTION("208. DTR_DUAL_I/O_FAST_READ with 3-byte address 2-2-2 DTR") {
            read_flash_test<3, 4>(FlashCmd::DtrDualInputOutputFastRead);
        }

        SECTION("209. ERASE_SECTOR with 3-byte address 2-2-0") {
            erase_flash_test<3>(FlashCmd::EraseSector);
        }

        SECTION("210. ERASE_SUBSECTOR_32KB with 3-byte address 2-2-0") {
            erase_flash_test<3>(FlashCmd::EraseSubsector32KB);
        }

        SECTION("211. ERASE_SUBSECTOR_4KB with 3-byte address 2-2-0") {
            erase_flash_test<3>(FlashCmd::EraseSubsector4KB);
        }

        SECTION("212. ERASE_DIE with 3-byte address 2-2-0") {
            erase_flash_test<3>(FlashCmd::EraseDie);
        }

        SECTION("213. PROGRAM_PAGE with 3-byte address 2-2-2") {
            program_flash_test<3>(FlashCmd::ProgramPage);
        }

        SECTION("250. 4-BYTE ADDRESS MODE") {
            enter_4bytes();

            SECTION("251. DUAL_OUTPUT_FAST_READ_4BYTE with 4-byte address 2-2-2 STR") {
                read_flash_test<4, 8>(FlashCmd::DualOutputFastRead4Byte);
            }

            SECTION("252. DUAL_I/O_FAST_READ_4BYTE with 4-byte address 2-2-2 STR") {
                read_flash_test<4, 4>(FlashCmd::DualInputOutputFastRead4Byte);
            }

            SECTION("253. DTR_DUAL_I/O_FAST_READ_4BYTE with 4-byte address 2-2-2 DTR") {
                read_flash_test<4, 4>(FlashCmd::DtrDualInputOutputFastRead4Byte);
            }

            SECTION("254. ERASE_SECTOR_4BYTE with 4-byte address 2-2-0") {
                erase_flash_test<4>(FlashCmd::EraseSector4Byte);
            }

            SECTION("255. ERASE_SUBSECTOR_32KB_4BYTE with 4-byte address 2-2-0") {
                erase_flash_test<4>(FlashCmd::EraseSubsector32KB4Byte);
            }

            SECTION("256. ERASE_SUBSECTOR_4KB_4BYTE with 4-byte address 2-2-0") {
                erase_flash_test<4>(FlashCmd::EraseSubsector4KB4Byte);
            }

            SECTION("257. PROGRAM_PAGE_4BYTE with 4-byte address 2-2-2") {
                program_flash_test<4>(FlashCmd::ProgramPage4Byte);
            }

            SECTION("290. EXIT 4 BYTES 2-0-0") {
                exit_4bytes();
            }
        }
    }
}

// A dedicated top-level SystemC wrapper module to manage the testbench lifecycle safely
class SystemCTestRunner : public sc_core::sc_module {
public:
    int& m_argc;
    char** m_argv;
    int m_result{0};

    // Instantiate your hardware submodules directly as class data members!
    FlashModel<MT25QU02GCBB> m_flash;
    Testbench  m_tb;

    SC_HAS_PROCESS(SystemCTestRunner);

    SystemCTestRunner(sc_core::sc_module_name name, int& argc, char** argv) 
        : sc_core::sc_module(name), m_argc(argc), m_argv(argv),
          m_flash("Flash_U1"),  // Kernel registers this as SystemC_Catch2_Bridge.Flash_U1
          m_tb("TB_I1")         // Kernel registers this as SystemC_Catch2_Bridge.TB_I1
    {
        // Bind the sockets cleanly inside the legitimate SystemC constructor window
        m_tb.initiator_socket(m_flash.flash_socket);
        
        g_flash_device = &m_flash;
        g_tb = &m_tb;

        // Spawn a main thread to run the tests once elaboration wraps up
        SC_THREAD(run_catch2_session);
    }

private:
    void run_catch2_session() {
        // Now that the sockets are successfully bound and the SystemC kernel is stable,
        // we can safely launch the Catch2 test engine runner!
        m_result = Catch::Session().run(m_argc, m_argv);
        
        sc_core::sc_stop(); // Gracefully shut down the SystemC scheduler loop
    }
};

int sc_main(int argc, char* argv[]) {
    // Seed the random number generator for any randomized test data generation
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    // 1. Explicitly silence INFO logs to clean up your testing terminal
    sc_core::sc_report_handler::set_actions(sc_core::SC_INFO, sc_core::SC_DO_NOTHING);

    // 2. FORCE Errors to immediately print to standard error stream and trigger a break
    sc_core::sc_report_handler::set_actions(
        sc_core::SC_ERROR, SC_DISPLAY | SC_LOG);
    
    // 3. FORCE Fatal errors to immediately print and abort execution 
    sc_core::sc_report_handler::set_actions(
        sc_core::SC_FATAL, SC_UNSPECIFIED);

    // Instantiate the primary test coordinator module
    SystemCTestRunner runner("SystemC_Catch2_Bridge", argc, argv);

    // Fire up the SystemC kernel engine. This triggers elaboration, binds ports,
    // and invokes the Catch2 suite on a safe worker thread thread channel.
    sc_core::sc_start();

    // Return Catch2's internal exit codes directly back to your terminal shell
    return runner.m_result;
}
