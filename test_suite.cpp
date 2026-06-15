#define CATCH_CONFIG_RUNNER  
#include <catch2/catch.hpp>

#include <memory.h>
#include "flash.h"
#include "testbench.h"

// Declare global unique_ptrs so they are accessible to Catch2 test cases
// but live safely managed across the SystemC lifetime loop
inline FlashModel<MT25QU02GCBB>* g_flash_device = nullptr;
inline Testbench*  g_tb = nullptr;


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
