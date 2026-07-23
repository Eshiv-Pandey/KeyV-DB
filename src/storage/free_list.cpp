#include "storage/free_list.h"

#include <cassert>
#include <cstring>
#include <stdexcept>

namespace keyvdb {

FreeList::FreeList(DiskManager& dm)
    : dm_(dm)
{}

void FreeList::load() {
    if (dm_.num_pages() == 0) {
        // Brand-new database: allocate page 0 and write a zeroed free list.
        page_id_t p0 = dm_.allocate_page();
        assert(p0 == 0 && "Page 0 must be the very first allocation");
        (void)p0;

        Page page;
        page.set_page_id(0);
        page.set_page_type(PageType::DB_HEADER);

        // count = 0, no entries.
        uint32_t count = 0;
        std::memcpy(page.data(), &count, sizeof(count));

        // Initialise the root ID slot at the tail to INVALID_PAGE_ID.
        // Without this, it defaults to 0 (all-zeros), which is a valid page ID
        // and would cause DB::Open to mistake a fresh DB for an existing one.
        page_id_t invalid_root = INVALID_PAGE_ID;
        std::memcpy(page.data() + ROOT_ID_OFFSET, &invalid_root, sizeof(invalid_root));

        dm_.write_page(0, page);
        dm_.flush();
        return;
    }

    // Existing database: read page 0 and decode the free list.
    Page page;
    dm_.read_page(0, page);

    uint32_t count = 0;
    std::memcpy(&count, page.data(), sizeof(count));

    if (count > MAX_FREE_PAGES) {
        throw std::runtime_error(
            "FreeList::load: corrupt count " + std::to_string(count) +
            " exceeds MAX_FREE_PAGES " + std::to_string(MAX_FREE_PAGES));
    }

    free_pages_.clear();
    free_pages_.resize(count);
    if (count > 0) {
        std::memcpy(free_pages_.data(),
                    page.data() + sizeof(uint32_t),
                    count * sizeof(page_id_t));
    }
}

void FreeList::save() {
    assert(dm_.num_pages() > 0 &&
           "FreeList::save() called before page 0 exists — did you call load()?");

    // Read-modify-write: read the current page 0 so we preserve any other
    // data stored there (specifically the root page ID slot at ROOT_ID_OFFSET).
    // Writing a blank Page and only filling in count+entries would silently
    // clobber the root ID that set_root_page_id() wrote.
    Page page;
    dm_.read_page(0, page);

    auto count = static_cast<uint32_t>(free_pages_.size());
    assert(count <= MAX_FREE_PAGES && "FreeList overflow — logic error");

    char* dst = page.data();
    std::memcpy(dst, &count, sizeof(count));
    dst += sizeof(uint32_t);
    if (count > 0) {
        std::memcpy(dst, free_pages_.data(), count * sizeof(page_id_t));
    }

    dm_.write_page(0, page);
    dm_.flush();
}

page_id_t FreeList::allocate() {
    if (free_pages_.empty()) {
        return INVALID_PAGE_ID;
    }
    page_id_t id = free_pages_.back();
    free_pages_.pop_back();
    save();
    return id;
}

void FreeList::free(page_id_t id) {
    assert(free_pages_.size() < MAX_FREE_PAGES &&
           "FreeList::free: list is full — should never happen in Milestone 1");
    free_pages_.push_back(id);
    save();
}

page_id_t FreeList::root_page_id() const {
    assert(dm_.num_pages() > 0 &&
           "FreeList::root_page_id() called before page 0 exists");

    Page page;
    dm_.read_page(0, page);

    page_id_t id = INVALID_PAGE_ID;
    std::memcpy(&id, page.data() + ROOT_ID_OFFSET, sizeof(id));
    return id;
}

void FreeList::set_root_page_id(page_id_t id) {
    assert(dm_.num_pages() > 0 &&
           "FreeList::set_root_page_id() called before page 0 exists");

    // We read the current page 0 so we don't clobber the free list entries,
    // write the root ID into the reserved slot at the tail, then write back.
    Page page;
    dm_.read_page(0, page);
    std::memcpy(page.data() + ROOT_ID_OFFSET, &id, sizeof(id));
    dm_.write_page(0, page);
    dm_.flush();
}

} // namespace keyvdb
