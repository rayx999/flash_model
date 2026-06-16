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
void plant_flash_data(const std::string& filepath, size_t offset, std::span<const uint8_t> data) {
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
        std::array<uint8_t, 5> spi_stream = { 
            static_cast<uint8_t>(FlashCmd::ReadSFDP), // Opcode
            0x00, 0x00, 0x00, // 3 bytes Address
            0x08        // 1 byte Dummy_cycles
        };

        std::array<uint8_t, 5> sfdp_signature = {
            'S', 'F', 'D', 'P'
        };

        auto status = g_tb->exchange_stream(spi_stream.data(), spi_stream.size());
        REQUIRE(status == tlm::TLM_OK_RESPONSE);
        REQUIRE(spi_stream == sfdp_signature); // Verifies that the entire stream was overwritten with the expected SFDP data

    }

    SECTION("3. FAST_READ with 3-byte address check") {
        uint32_t data_len = std::rand() % 256 + 1; // Random data length between 1 and 256 bytes
        uint32_t addr = std::rand() % (g_flash_device->get_capacity() - data_len); // Random address within bounds
        addr %= 0x1000000; // Ensure it fits within 3 bytes
        std::vector<uint8_t> golden_data(data_len);
        std::iota(golden_data.begin(), golden_data.end(), std::rand() % 256); // Fill with random values

        // Insert test data into flash backing file at the correct offset so the model can read it back during the test
        plant_flash_data(g_flash_device->get_back_file_name(), addr, golden_data);

        // 1. Calculate overall stream size: 
        // 1 byte (Opcode) + 3 bytes (Address) + 1 byte (Dummy Clock) + Variable Response Data Size
        size_t total_stream_size = 1 + 3 + 1 + data_len;
        std::vector<uint8_t> fast_read(total_stream_size, 0x00);

        // 2. Compose the structural SPI Command Stream protocol header
        fast_read[0] = static_cast<uint8_t>(FlashCmd::FastRead);
        fast_read[1] = static_cast<uint8_t>((addr >> 16) & 0xFF); // Address Byte 2 (MSB)
        fast_read[2] = static_cast<uint8_t>((addr >> 8) & 0xFF);  // Address Byte 1
        fast_read[3] = static_cast<uint8_t>(addr & 0xFF);         // Address Byte 0 (LSB)
        fast_read[4] = 0x08;                                      // Dummy clock byte cycle overhead

        // 3. Fire the execution stream over your SystemC simulation interface
        auto status = g_tb->exchange_stream(fast_read.data(), fast_read.size());
        
        // 4. Assertions
        REQUIRE(status == tlm::TLM_OK_RESPONSE);

        // FIXED: Isolate the readback payload window using a C++20 view to match against golden_data
        auto readback_window = std::span<const uint8_t>(fast_read.data() + 5, data_len);
        // Explicitly forces an element-by-element deep comparison 
        REQUIRE(std::equal(readback_window.begin(), readback_window.end(), golden_data.begin(), golden_data.end()));
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
