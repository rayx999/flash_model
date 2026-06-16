#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <systemc.h>

// Memory mapping maps the entire file contents directly into your process's virtual memory 
// address space for super-fast random access. This class encapsulates the logic for managing 
// a memory-mapped file as flash storage.

class FlashStorage : public sc_core::sc_module{
private:
    int m_fd{-1};
    uint8_t* m_file_ptr{nullptr};
    size_t m_size{0};

public:
    FlashStorage(sc_core::sc_module_name name) : sc_core::sc_module(name) {}

    std::string get_back_file_name() const {
        return "flash_backing_storage.bin";
    }
    
    bool open_storage(const std::string& filepath, size_t storage_bytes) {
        m_size = storage_bytes;
        // Open or create the backing storage file
        m_fd = ::open(filepath.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (m_fd < 0) return false;

        // Ensure the file is truncated to the correct physical size
        if (::ftruncate(m_fd, m_size) != 0) {
            ::close(m_fd);
            return false;
        }

        // Map the file into virtual address memory
        m_file_ptr = static_cast<uint8_t*>(::mmap(
            nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0
        ));

        return m_file_ptr != nullptr;
    }

    // Direct O(1) random-access write block
    void write_byte(size_t offset, uint8_t value) noexcept {
        if (offset < m_size) [[likely]] {
            m_file_ptr[offset] = value;
        }
    }

    // Direct O(1) random-access read block
    uint8_t read_byte(size_t offset) const noexcept {
        if (offset < m_size) [[likely]] {
            return m_file_ptr[offset];
        }
        return 0xFF; // Unprogrammed flash default state
    }

        // Direct O(1) random-access read block
    int read( uint8_t* stream, const uint32_t addr, const size_t len) const noexcept {
        std::copy_n(m_file_ptr + addr, len, stream);
        return len; 
    }

    ~FlashStorage() {
        if (m_file_ptr != nullptr) ::munmap(m_file_ptr, m_size);
        if (m_fd >= 0) ::close(m_fd);
    }
};
