#include <span>
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
int FlashModel<T>::process_flash_cmd(CommandTraits& traits, uint8_t* stream, size_t len) noexcept {
    int ret = 0; // Default to success

    switch (traits.cmd) {
        case FlashCmd::ResetEnable:
            m_reset_enabled = true;
            break;
        case FlashCmd::ResetMemory:
            if (m_reset_enabled) {
                ret = reset_memory();
            } else {
                ret = -1;
            }
            break;
        case FlashCmd::ReadId:
            ret = read_id(stream, len);
            break;
        case FlashCmd::ReadSFDP:
            ret = read_sfdp(traits, stream, len); // You can expand this to handle the address and dummy cycles properly
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
        case FlashCmd::ReadVolatileConfigure:
            ret = read_register(stream, len);
            break;
        case FlashCmd::WriteVolatileConfigure:
            ret = write_register(stream, len);
            break;
        case FlashCmd::Read:
        case FlashCmd::FastRead:
        case FlashCmd::DtrFastRead:        
        case FlashCmd::QuadOutputFastRead:
        case FlashCmd::DtrQuadOutputFastRead:        
        case FlashCmd::QuadInputOutputFastRead:
        case FlashCmd::DtrQuadInputOutputFastRead:
        case FlashCmd::Read4Byte:
        case FlashCmd::FastRead4Byte:
        case FlashCmd::DtrFastRead4Byte:        
        case FlashCmd::QuadOutputFastRead4Byte:
        case FlashCmd::QuadInputOutputFastRead4Byte:
        case FlashCmd::DtrQuadInputOutputFastRead4Byte:        
            ret = read_flash(traits, stream, len); // You can expand this to handle the address and dummy cycles properly
            break;

        case FlashCmd::EraseSector:
        case FlashCmd::EraseSubsector4KB:
        case FlashCmd::EraseSubsector32KB:
        case FlashCmd::EraseDie:
        case FlashCmd::EraseSector4Byte:
        case FlashCmd::EraseSubsector4KB4Byte:
        case FlashCmd::EraseSubsector32KB4Byte:
            ret = erase_flash(traits, stream, len);
            break;

        case FlashCmd::ProgramPage:
        case FlashCmd::QuadInputFastProgram:
        case FlashCmd::ExtendedQuadInputFastProgram:
        case FlashCmd::ProgramPage4Byte:
        case FlashCmd::QuadInputFastProgram4Byte:
        case FlashCmd::ExtendedQuadInputFastProgram4Byte:
            ret = program_flash(traits, stream, len);
            break;

        default:
            // Handle further program/erase sequences directly
            ret = -1; // Indicate an error for unhandled but valid commands
            SC_REPORT_ERROR("FlashModel", make_msg(
                "Received a Flash Command code that is recognized but not yet implemented in the command processor. Command: 0x%02X",
                static_cast<uint8_t>(traits.cmd)).c_str()); 
            break;
    }
    return ret;
}

template <typename T>
int FlashModel<T>::reset_memory() noexcept {
    T::profile.config = FlashDeviceConfig();
    m_reset_enabled = false;
    m_write_enabled = false;

    SC_REPORT_INFO("FlashModel", "Reset_Memory successful.");
    return 0;    
}

template <typename T>
int FlashModel<T>::read_id(uint8_t* stream, size_t len) noexcept {
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
int FlashModel<T>::read_sfdp(const CommandTraits& traits, uint8_t* stream, size_t len) noexcept {
    uint32_t addr = get_addr(stream + 1, 3); // SFDP read always assume 3 bytes address after the opcode
    uint32_t dummy_cycles = stream[4]; //
    uint32_t hdr_len = 1 + 3 + 1; // Opcode + Address Bytes + Dummy Cycles Byte 

    if (dummy_cycles != get_dummy_clocks<T>(traits)) {
        SC_REPORT_WARNING("FlashModel", make_msg(
            "Provided dummy cycles %d do not match the expected value %d for this command.", 
            dummy_cycles, get_dummy_clocks<T>(traits)).c_str());
    }

    if (addr + len > T::profile.sfdp.size()) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Read_SFDP failed. Address 0x%x plus length 0x%x exceeds available SFDP data 0x%x.",
            addr, len, T::profile.sfdp.size()).c_str());
        return -1; // Indicate an error if the requested range is out of bounds
    }

    std::memcpy(stream + hdr_len, T::profile.sfdp.data() + addr, len); // Copy the requested SFDP data into the stream buffer
    SC_REPORT_INFO("FlashModel", "Read_SFDP successful.");
    return 0;
}

template <typename T>
int FlashModel<T>::read_register(uint8_t* stream, size_t len) noexcept {
    if (len < 3) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Read_Register failed. Provided buffer length %d is too small to hold the 2 bytes register.",
            len).c_str());
        return -1; // Indicate an error if the buffer is too small to hold the command header
    }    

    uint16_t raw = std::bit_cast<uint16_t>(T::profile.config);
    stream[1] = raw >> 8;
    stream[2] = raw & 0xFF;

    SC_REPORT_INFO("FlashModel", "Read_Register successful.");
    return 0;
}

template <typename T>
int FlashModel<T>::write_register(uint8_t* stream, size_t len) noexcept {
    if (!m_write_enabled) {
         SC_REPORT_ERROR("FlashModel", make_msg(
            "Read_Wite failed. Write NOT enabled.").c_str());
        return -1; // Indicate an error if the buffer is too small to hold the command header       
    }

    if (len < 3) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Read_Wite failed. Provided buffer length %d is too small to hold the 2 bytes register.",
            len).c_str());
        return -1; // Indicate an error if the buffer is too small to hold the command header
    }    

    uint16_t raw = (static_cast<uint16_t>(stream[1] << 8)) | stream[2];
    T::profile.config = std::bit_cast<FlashDeviceConfig>(raw);

    SC_REPORT_INFO("FlashModel", "Write_Register successful.");
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

    if (dummy_cycles != get_dummy_clocks<T>(traits)) [[unlikely]] {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Provided dummy cycles %d do not match the expected dummy cycles %d for this command 0x%02X.", 
            dummy_cycles, get_dummy_clocks<T>(traits), traits.cmd).c_str());
        return -1;
    }

    if (addr + len > get_capacity()) [[unlikely]] {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Read_Flash failed. Address 0x%x plus length 0x%x exceeds flash capacity 0x%x.",
            addr, len, get_capacity()).c_str());
        return -1; // Indicate an error if the requested range is out of bounds
    }

    flash_storage.read(stream + hdr_len, addr, len - hdr_len); // Read the requested flash data into the stream buffer
    SC_REPORT_INFO("FlashModel", "Read_Flash successful.");
    return 0;
}

template <typename T>
int FlashModel<T>::erase_flash(const CommandTraits& traits, uint8_t* stream, size_t len) noexcept {
    if (!m_write_enabled) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Erase_Flash failed. write_enable is false.").c_str());
        return -1;
    }

    AddressBytes address_len = T::get_addr_len();
    if (traits.requested_address_bytes != AddressBytes::ADDR_LEN_ANY &&
        traits.requested_address_bytes != address_len) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Erase_Flash failed. Command requires %d address bytes but current address len %d.",
            traits.requested_address_bytes, address_len).c_str());
        return -1; // Indicate an error if the address length does not match the command's requirement
    }

    uint32_t addr_len = static_cast<uint32_t>(address_len);
    uint32_t hdr_len = 1 + addr_len + 1; // Opcode + Address Bytes + Dummy Cycles Byte 
    if (len < hdr_len) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Erase_Flash failed. Provided buffer length %d is too small to hold the command header of %d bytes.",
            len, hdr_len).c_str());
        return -1; // Indicate an error if the buffer is too small to hold the command header
    }

    size_t addr = get_addr(stream + 1, addr_len); // Read the address bytes based on the command traits
    size_t size = get_erase_size(traits.cmd);
    size_t start = (addr & ~(size - 1)); // mask to the beginning of erase unit

    if (start + size > get_capacity()) [[unlikely]] {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Erase_Flash failed. Address 0x%x plus length 0x%x exceeds flash capacity 0x%x.",
            addr, size, get_capacity()).c_str());
        return -1; // Indicate an error if the requested range is out of bounds
    }

    std::vector<uint8_t> buf(size, 0xFF);
    flash_storage.write(buf.data(), start, size); // Read the requested flash data into the stream buffer

    SC_REPORT_INFO("FlashModel", "Erase_Flash successful.");
    return 0;
}

template <typename T>
int FlashModel<T>::program_flash(const CommandTraits& traits, uint8_t* stream, size_t len) noexcept {
    if (!m_write_enabled) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Program_Flash failed. write_enable is false.").c_str());
        return -1;
    }

    AddressBytes address_len = T::get_addr_len();
    if (traits.requested_address_bytes != AddressBytes::ADDR_LEN_ANY &&
        traits.requested_address_bytes != address_len) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Program_Flash failed. Command requires %d address bytes but current address len %d.",
            traits.requested_address_bytes, address_len).c_str());
        return -1; // Indicate an error if the address length does not match the command's requirement
    }

    uint32_t addr_len = static_cast<uint32_t>(address_len);
    uint32_t hdr_len = 1 + addr_len; // Opcode + Address Bytes 
    if (len < hdr_len) {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Program_Flash failed. Provided buffer length %d is too small to hold the command header of %d bytes.",
            len, hdr_len).c_str());
        return -1; // Indicate an error if the buffer is too small to hold the command header
    }

    size_t addr = get_addr(stream + 1, addr_len); // Read the address bytes based on the command traits
    if (addr + len - hdr_len > get_capacity()) [[unlikely]] {
        SC_REPORT_ERROR("FlashModel", make_msg(
            "Program_Flash failed. Address 0x%x plus length 0x%x exceeds flash capacity 0x%x.",
            addr, len - hdr_len, get_capacity()).c_str());
        return -1; // Indicate an error if the requested range is out of bounds
    }

    auto wdata = std::span<uint8_t>(stream + hdr_len, len - hdr_len);
    flash_storage.write(wdata.data(), addr, wdata.size()); // Read the requested flash data into the stream buffer

    SC_REPORT_INFO("FlashModel", "Program_Flash successful.");
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
