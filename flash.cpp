#include "flash.h"

// Phase-Stream Mapping to map all flash transaction by TLM_WRITE
// The Payload Data Buffer: The data payload pointer (data_ptr) points to a buffer containing 
// the raw byte stream as it would appear on the physical SPI wire 
// (e.g., [Opcode, Addr0, Addr1, Addr2, Dummy, Data0, Data1...]).
// Bi-directional data: For read commands, the initiator creates an empty buffer,
// writes the opcode/address at the front, and the target Flash model overwrites the trailing 
// data bytes directly inside that same buffer before returning.
template <typename T>
void FlashModel<T>::b_transport(tlm::tlm_generic_payload& trans, [[maybe_unused]] sc_core::sc_time& delay) noexcept {
    uint8_t*         data_ptr = trans.get_data_ptr();
    unsigned int     length   = trans.get_data_length();

    if (length == 0 || data_ptr == nullptr) {
        trans.set_response_status(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
        return;
    }

    // Check for valid flash command types first
    FlashCmd op = static_cast<FlashCmd>(data_ptr[0]);
    auto traits_opt = get_traits<T>(op);
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
template <typename T>
int FlashModel<T>::process_flash_cmd(CommandTraits& traits, uint8_t* stream, unsigned int len) noexcept {
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
        case FlashCmd::ReadSFDP:
            ret = read_sfdp(stream, len, traits.dummy_clocks_str); // You can expand this to handle the address and dummy cycles properly
            break;
        case FlashCmd::WriteEnable:
                m_write_enabled = true;
            break;
        case FlashCmd::WriteDisable:
                m_write_enabled = false;
            break;
        case FlashCmd::Enter4Byte:
        case FlashCmd::Exit4Byte:
            if (m_write_enabled) {
                T::profile.config.addr_len = (traits.cmd == FlashCmd::Enter4Byte) ? 0 : 1;
                // Implement the logic to switch between 3-byte and 4-byte address modes here
                SC_REPORT_INFO("FlashModel", make_msg(
                    "Address mode switch command %s executed successfully.",
                    traits.cmd == FlashCmd::Enter4Byte ? "Enter4Byte" : "Exit4Byte").c_str());
            } 
            break;
        case FlashCmd::Read:
        case FlashCmd::FastRead:
        case FlashCmd::Read4Byte:
            ret = read_flash(traits, stream, len); // You can expand this to handle the address and dummy cycles properly
            break;

        default:
            // Handle further program/erase sequences directly
            ret = -1; // Indicate an error for unhandled but valid commands
            break;
    }
    return ret;
}

template <typename T>
int FlashModel<T>::read_id(uint8_t* stream, unsigned int len) noexcept {
    // Populate the data stream with the expected JEDEC ID values
    if (len >= 3) {
        stream[0] = T::profile.jedec_id[0]; // Manufacturer ID 
        stream[1] = T::profile.jedec_id[1]; // Memory Type: 
        stream[2] = T::profile.jedec_id[2]; // Capacity 

        SC_REPORT_INFO("FlashModel", "Read_ID successful.");
        return 0;
    }

    SC_REPORT_ERROR("FlashModel", "Read_ID failed. Provided buffer is too small to hold the JEDEC ID.");
    return -1; // Indicate an error if the buffer is too small
}

template <typename T>
int FlashModel<T>::read_sfdp(uint8_t* stream, unsigned int len, uint32_t dummy_clocks) noexcept {
    uint32_t addr = get_addr(stream + 1, 3); // SFDP read always assume 3 bytes address after the opcode
    uint32_t dummy_cycles = stream[4]; //
    
    if (dummy_cycles != dummy_clocks) {
        SC_REPORT_WARNING("FlashModel", make_msg(
            "Provided dummy cycles %d do not match the expected value %d for this command.", 
            dummy_cycles, dummy_clocks).c_str());
    }

    if (addr + len > T::profile.sfdp.size()) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Read_SFDP failed. Address 0x%x plus length 0x%x exceeds available SFDP data 0x%x.",
            addr, len, T::profile.sfdp.size()).c_str());
        return -1; // Indicate an error if the requested range is out of bounds
    }

    std::memcpy(stream, T::profile.sfdp.data() + addr, len); // Copy the requested SFDP data into the stream buffer
    SC_REPORT_INFO("FlashModel", "Read_SFDP successful.");
    return 0;
}

template <typename T>
int FlashModel<T>::read_flash(const CommandTraits& traits, uint8_t* stream, size_t len) noexcept {
    AddressBytes address_len = T::get_addr_len();
    if (traits.requested_address_bytes != AddressBytes::ADDR_LEN_ANY &&
        traits.requested_address_bytes != address_len) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Read_Flash failed. Command requires %d address bytes but current address len %d.",
            traits.requested_address_bytes, address_len).c_str());
        return -1; // Indicate an error if the address length does not match the command's requirement
    }

    uint32_t addr_len = static_cast<uint32_t>(address_len);
    uint32_t hdr_len = 1 + addr_len + 1; // Opcode + Address Bytes + Dummy Cycles Byte 
    if (len <= hdr_len) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Read_Flash failed. Provided buffer length %d is too small to hold the command header of %d bytes.",
            len, hdr_len).c_str());
        return -1; // Indicate an error if the buffer is too small to hold the command header
    }

    uint32_t addr = get_addr(stream + 1, addr_len); // Read the address bytes based on the command traits
    uint32_t dummy_cycles = stream[1 + addr_len]; // Read the dummy cycles byte that follows the address bytes

    BusProtocol protocol = T::get_bus_protocol();
    if (protocol != traits.protocol) [[unlikely]] {
        SC_REPORT_WARNING("FlashModel", make_msg(
            "Provided bus protocol %d do not match the expected value %d for this command.", 
            protocol, traits.protocol).c_str());
    }

    TransferRate transfer_rate = T::get_transfer_rate();
    if (transfer_rate == TransferRate::STR && dummy_cycles != traits.dummy_clocks_str) [[unlikely]] {
        SC_REPORT_WARNING("FlashModel", make_msg(
            "Provided dummy cycles %d do not match the expected value %d for this command.", 
            dummy_cycles, traits.dummy_clocks_str).c_str());
    }

    if (transfer_rate == TransferRate::DTR && dummy_cycles != traits.dummy_clocks_dtr) [[unlikely]] {
        SC_REPORT_WARNING("FlashModel", make_msg(
            "Provided dummy cycles %d do not match the expected value %d for this command.", 
            dummy_cycles, traits.dummy_clocks_dtr).c_str());
    }

    if (addr + len > T::profile.capacity_bytes) [[unlikely]] {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Read_Flash failed. Address 0x%x plus length 0x%x exceeds flash capacity 0x%x.",
            addr, len, T::profile.capacity_bytes).c_str());
        return -1; // Indicate an error if the requested range is out of bounds
    }

    flash_storage.read(stream + hdr_len, addr, len - hdr_len); // Read the requested flash data into the stream buffer
    SC_REPORT_INFO("FlashModel", "Read_Flash successful.");
    return 0;
}

// ============================================================================
// Explicit Template Instantiations
// ============================================================================
// This forces the compiler to build the binary objects for these specific 
// configurations right inside flash.cpp, making them visible to the linker.

template class FlashModel<MT25QU02GCBB>;

// If you add other profiles later (like Winbond), register them here too:
// template class FlashModel<W25Q128>;
