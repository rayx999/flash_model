#pragma once

#include <systemc.h>
#include <tlm.h>
#include "flash.h"

class Testbench : public sc_core::sc_module, public tlm::tlm_bw_transport_if<> {
public:
    tlm::tlm_initiator_socket<32> initiator_socket;

    SC_HAS_PROCESS(Testbench);
    explicit Testbench(sc_core::sc_module_name name) : sc_module(name), initiator_socket("initiator_socket")
    {
        // THE REAL FIX: Bind the socket's backward path to THIS class!
        // This gives the internal sc_export an interface and clears E120.
        initiator_socket.bind(*this);         
    }

    // Provide mandatory dummy implementations for the backward path functions
    void invalidate_direct_mem_ptr(sc_dt::uint64, sc_dt::uint64) override {}
    tlm::tlm_sync_enum nb_transport_bw(tlm::tlm_generic_payload&, tlm::tlm_phase&, sc_core::sc_time&) override {
        return tlm::TLM_COMPLETED;
    }

    // Sends a unified stream down the simple socket interface
    tlm::tlm_response_status exchange_stream(uint8_t* buffer, unsigned int len) {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        tlm::tlm_generic_payload payload;

        payload.set_command(tlm::TLM_WRITE_COMMAND); // Driven as a wire stream write
        payload.set_address(0);
        payload.set_data_ptr(buffer);
        payload.set_data_length(len);

        initiator_socket->b_transport(payload, delay);
        return payload.get_response_status();
    }
};

