#include "storage/disk_manager.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace keyvdb {

DiskManager::DiskManager(const std::string& db_path)
    : db_path_(db_path)
{
    fd_ = ::open(db_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "DiskManager: failed to open " + db_path);
    }

    // Compute num_pages_ from the actual file size so that reopening an
    // existing database returns the correct page count. Initialising to 0
    // would break PersistAcrossReopen and any future recovery logic.
    off_t file_size = ::lseek(fd_, 0, SEEK_END);
    if (file_size < 0) {
        ::close(fd_);
        throw std::system_error(errno, std::generic_category(),
                                "DiskManager: lseek(SEEK_END) failed");
    }
    num_pages_ = static_cast<page_id_t>(file_size / PAGE_SIZE);
}

DiskManager::~DiskManager() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void DiskManager::read_page(page_id_t page_id, Page& page) {
    if (page_id < 0 || page_id >= num_pages_) {
        throw std::out_of_range(
            "DiskManager::read_page: page_id " + std::to_string(page_id) +
            " is out of range [0, " + std::to_string(num_pages_) + ")");
    }

    off_t    offset = static_cast<off_t>(page_id) * PAGE_SIZE;
    ssize_t  n      = ::pread(fd_, page.raw_data(), PAGE_SIZE, offset);

    if (n < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "DiskManager::read_page: pread failed");
    }
    if (static_cast<size_t>(n) != PAGE_SIZE) {
        throw std::runtime_error(
            "DiskManager::read_page: short read — expected " +
            std::to_string(PAGE_SIZE) + " bytes, got " + std::to_string(n));
    }
}

void DiskManager::write_page(page_id_t page_id, const Page& page) {
    if (page_id < 0) {
        throw std::invalid_argument(
            "DiskManager::write_page: negative page_id " +
            std::to_string(page_id));
    }

    off_t   offset = static_cast<off_t>(page_id) * PAGE_SIZE;
    ssize_t n      = ::pwrite(fd_, page.raw_data(), PAGE_SIZE, offset);

    if (n < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "DiskManager::write_page: pwrite failed");
    }
    if (static_cast<size_t>(n) != PAGE_SIZE) {
        throw std::runtime_error(
            "DiskManager::write_page: short write — expected " +
            std::to_string(PAGE_SIZE) + " bytes, wrote " + std::to_string(n));
    }
}

void DiskManager::flush() {
    if (::fsync(fd_) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "DiskManager::flush: fsync failed");
    }
}

page_id_t DiskManager::allocate_page() {
    page_id_t new_id = num_pages_;

    // Write a zeroed page at the new offset to extend the file and ensure the
    // page is properly initialised on disk. Using ftruncate would also extend
    // the file, but pwrite is consistent with our other I/O and explicitly
    // initialises the bytes rather than relying on sparse-file behaviour.
    Page zero_page;  // default-constructed → clear() in constructor → all zeros
    off_t   offset = static_cast<off_t>(new_id) * PAGE_SIZE;
    ssize_t n      = ::pwrite(fd_, zero_page.raw_data(), PAGE_SIZE, offset);

    if (n < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "DiskManager::allocate_page: pwrite failed");
    }

    ++num_pages_;
    return new_id;
}

} // namespace keyvdb
