#include "flash.h"

// Phase-Stream Mapping to map all flash transaction by TLM_WRITE
// The Payload Data Buffer: The data payload pointer (data_ptr) points to a buffer containing 
// the raw byte stream as it would appear on the physical SPI wire 
// (e.g., [Opcode, Addr0, Addr1, Addr2, Dummy, Data0, Data1...]).
// Bi-directional data: For read commands, the initiator creates an empty buffer,
// writes the opcode/address at the front, and the target Flash model overwrites the trailing 
// data bytes directly inside that same buffer before returning.
void FlashModel::b_transport(tlm::tlm_generic_payload& trans, [[maybe_unused]] sc_core::sc_time& delay) noexcept {
    uint8_t*         data_ptr = trans.get_data_ptr();
    unsigned int     length   = trans.get_data_length();

    if (length == 0 || data_ptr == nullptr) {
        trans.set_response_status(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
        return;
    }

    // Check for valid flash command types first
    FlashCmd incoming_op = static_cast<FlashCmd>(data_ptr[0]);
    auto traits_opt = get_traits(incoming_op);
    if (!traits_opt.has_value()) {
        // Handle the invalid command error immediately!
        SC_REPORT_ERROR("FlashModel", "Received an invalid or unassigned Flash Command code.");
        trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        return;
    }
    
    // Assume all tlm::TLM_WRITE commands on SPI transactions
    int ret = process_flash_cmd(traits_opt.value(), data_ptr, length);
    trans.set_response_status(ret == 0 ? tlm::TLM_OK_RESPONSE : tlm::TLM_GENERIC_ERROR_RESPONSE);
}

// Command controller execution unit
int FlashModel::process_flash_cmd(CommandTraits& traits, uint8_t* stream, unsigned int len) noexcept {
    int ret = 0; // Default to success

    switch (traits.cmd) {
        case FlashCmd::ResetEnable:
            m_reset_enabled = true;
            break;
        case FlashCmd::ResetMemory:
            if (m_reset_enabled) {
                m_reset_enabled = false;
                SC_REPORT_INFO("FlashModel", "Software Reset Memory sequence successful.");
            }
            break;
        case FlashCmd::ReadId:
            ret = read_id(stream, len);
            break;
        case FlashCmd::FastRead:
            ret = read_flash(stream, len);
            break;

        default:
            // Handle further program/erase sequences directly
            ret = -1; // Indicate an error for unhandled but valid commands
            break;
    }
    return ret;
}

int FlashModel::read_id(uint8_t* stream, unsigned int len) noexcept {
    // Populate the data stream with the expected JEDEC ID values
    if (len >= 3) {
        stream[0] = 0x20; // Manufacturer ID (Micron)
        stream[1] = 0xBB; // Memory Type: 1.8v
        stream[2] = 0x22; // Capacity (2Gb)

        SC_REPORT_INFO("FlashModel", "Read_ID successful.");
        return 0;
    }

    SC_REPORT_ERROR("FlashModel", "Read_ID failed. Provided buffer is too small to hold the JEDEC ID.");
    return -1; // Indicate an error if the buffer is too small
}

int FlashModel::read_flash(uint8_t* stream, unsigned int len) noexcept {
    // This is a placeholder for the actual read logic, which would involve
    // interpreting the address and dummy cycles from the command stream,
    // then populating the data stream with the requested flash memory contents.
    return 0;
}
