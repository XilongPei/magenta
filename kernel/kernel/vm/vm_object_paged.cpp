// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "kernel/vm/vm_object.h"

#include "vm_priv.h"

#include <arch/ops.h>
#include <assert.h>
#include <err.h>
#include <inttypes.h>
#include <kernel/auto_lock.h>
#include <kernel/vm.h>
#include <kernel/vm/vm_address_region.h>
#include <lib/console.h>
#include <lib/user_copy.h>
#include <new.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>

#define LOCAL_TRACE MAX(VM_GLOBAL_TRACE, 0)

namespace {

void ZeroPage(paddr_t pa) {
    void* ptr = paddr_to_kvaddr(pa);
    DEBUG_ASSERT(ptr);

    arch_zero_page(ptr);
}

void ZeroPage(vm_page_t* p) {
    paddr_t pa = vm_page_to_paddr(p);
    ZeroPage(pa);
}

} // namespace

VmObjectPaged::VmObjectPaged(uint32_t pmm_alloc_flags)
    : pmm_alloc_flags_(pmm_alloc_flags) {
    LTRACEF("%p\n", this);
}

VmObjectPaged::~VmObjectPaged() {
    DEBUG_ASSERT(magic_ == MAGIC);
    LTRACEF("%p\n", this);

    // free all of the pages attached to us
    page_list_.FreeAllPages();
}

mxtl::RefPtr<VmObject> VmObjectPaged::Create(uint32_t pmm_alloc_flags, uint64_t size) {
    // there's a max size to keep indexes within range
    if (size > MAX_SIZE)
        return nullptr;

    AllocChecker ac;
    auto vmo = mxtl::AdoptRef<VmObject>(new (&ac) VmObjectPaged(pmm_alloc_flags));
    if (!ac.check())
        return nullptr;

    auto err = vmo->Resize(size);
    if (err == ERR_NO_MEMORY)
        return nullptr;
    // Other kinds of failures are not handled yet.
    DEBUG_ASSERT(err == NO_ERROR);
    if (err != NO_ERROR)
        return nullptr;

    return vmo;
}

void VmObjectPaged::Dump(uint depth, bool verbose) {
    if (magic_ != MAGIC) {
        printf("VmObjectPaged at %p has bad magic\n", this);
        return;
    }

    AutoLock a(lock_);

    size_t count = 0;
    page_list_.ForEveryPage([&count](const auto p, uint64_t) { count++; });

    for (uint i = 0; i < depth; ++i) {
        printf("  ");
    }
    printf("object %p size %#" PRIx64 " pages %zu ref %d\n", this, size_, count, ref_count_debug());

    if (verbose) {
        auto f = [depth](const auto p, uint64_t offset) {
            for (uint i = 0; i < depth + 1; ++i) {
                printf("  ");
            }
            printf("offset %#" PRIx64 " page %p paddr %#" PRIxPTR "\n", offset, p, vm_page_to_paddr(p));
        };
        page_list_.ForEveryPage(f);
    }
}

size_t VmObjectPaged::AllocatedPages() const {
    DEBUG_ASSERT(magic_ == MAGIC);
    AutoLock a(lock_);
    size_t count = 0;
    page_list_.ForEveryPage([&count](const auto p, uint64_t) { count++; });
    return count;
}

status_t VmObjectPaged::AddPage(vm_page_t* p, uint64_t offset) {
    DEBUG_ASSERT(magic_ == MAGIC);
    LTRACEF("vmo %p, offset %#" PRIx64 ", page %p (%#" PRIxPTR ")\n", this, offset, p, vm_page_to_paddr(p));

    DEBUG_ASSERT(p);

    AutoLock a(lock_);

    if (offset >= size_)
        return ERR_OUT_OF_RANGE;

    return page_list_.AddPage(p, offset);
}

mxtl::RefPtr<VmObject> VmObjectPaged::CreateFromROData(const void* data, size_t size) {
    auto vmo = Create(PMM_ALLOC_FLAG_ANY, size);
    if (vmo && size > 0) {
        ASSERT(IS_PAGE_ALIGNED(size));
        ASSERT(IS_PAGE_ALIGNED(reinterpret_cast<uintptr_t>(data)));

        // Do a direct lookup of the physical pages backing the range of
        // the kernel that these addresses belong to and jam them directly
        // into the VMO.
        //
        // NOTE: This relies on the kernel not otherwise owning the pages.
        // If the setup of the kernel's address space changes so that the
        // pages are attached to a kernel VMO, this will need to change.

        paddr_t start_paddr = vaddr_to_paddr(data);
        ASSERT(start_paddr != 0);

        for (size_t count = 0; count < size / PAGE_SIZE; count++) {
            paddr_t pa = start_paddr + count * PAGE_SIZE;
            vm_page_t* page = paddr_to_vm_page(pa);
            ASSERT(page);

            if (page->state == VM_PAGE_STATE_WIRED) {
                // it's wired to the kernel, so we can just use it directly
            } else if (page->state == VM_PAGE_STATE_FREE) {
                ASSERT(pmm_alloc_range(pa, 1, nullptr) == 1);
                page->state = VM_PAGE_STATE_WIRED;
            } else {
                panic("page used to back static vmo in unusable state: paddr %#" PRIxPTR " state %u\n", pa,
                      page->state);
            }

            // XXX hack to work around the ref pointer to the base class
            auto vmo2 = static_cast<VmObjectPaged*>(vmo.get());
            vmo2->AddPage(page, count * PAGE_SIZE);
        }

        // TODO(mcgrathr): If the last reference to this VMO were released
        // so the VMO got destroyed, that would attempt to return these
        // pages to the system.  On arm and arm64, the kernel cannot
        // tolerate a hole being created in the kernel image mapping, so
        // bad things happen.  Until that issue is fixed, just leak a
        // reference here so that the new VMO will never be destroyed.
        vmo.reset(vmo.leak_ref());
    }

    return vmo;
}

vm_page_t* VmObjectPaged::GetPageLocked(uint64_t offset) {
    DEBUG_ASSERT(magic_ == MAGIC);
    DEBUG_ASSERT(lock_.IsHeld());

    if (offset >= size_)
        return nullptr;

    return page_list_.GetPage(offset);
}

vm_page_t* VmObjectPaged::FaultPageLocked(uint64_t offset, uint pf_flags) {
    DEBUG_ASSERT(magic_ == MAGIC);
    DEBUG_ASSERT(lock_.IsHeld());

    LTRACEF("vmo %p, offset %#" PRIx64 ", pf_flags %#x\n", this, offset, pf_flags);

    if (offset >= size_)
        return nullptr;

    vm_page_t* p = page_list_.GetPage(offset);
    if (p)
        return p;

    // allocate a page
    paddr_t pa;
    p = pmm_alloc_page(pmm_alloc_flags_, &pa);
    if (!p)
        return nullptr;

    p->state = VM_PAGE_STATE_OBJECT;

    // TODO: remove once pmm returns zeroed pages
    ZeroPage(pa);

    __UNUSED auto status = page_list_.AddPage(p, offset);
    DEBUG_ASSERT(status == NO_ERROR);

    LTRACEF("faulted in page %p, pa %#" PRIxPTR "\n", p, pa);

    return p;
}

status_t VmObjectPaged::CommitRange(uint64_t offset, uint64_t len, uint64_t* committed) {
    DEBUG_ASSERT(magic_ == MAGIC);
    LTRACEF("offset %#" PRIx64 ", len %#" PRIx64 "\n", offset, len);

    if (committed)
        *committed = 0;

    AutoLock a(lock_);

    // trim the size
    if (!TrimRange(offset, len, size_))
        return ERR_OUT_OF_RANGE;

    // was in range, just zero length
    if (len == 0)
        return NO_ERROR;

    // compute a page aligned end to do our searches in to make sure we cover all the pages
    uint64_t end = ROUNDUP_PAGE_SIZE(offset + len);
    DEBUG_ASSERT(end > offset);

    // make a pass through the list, counting the number of pages we need to allocate
    size_t count = 0;
    for (uint64_t o = offset; o < end; o += PAGE_SIZE) {
        if (!page_list_.GetPage(o))
            count++;
    }
    if (count == 0)
        return NO_ERROR;

    // allocate count number of pages
    list_node page_list;
    list_initialize(&page_list);

    size_t allocated = pmm_alloc_pages(count, pmm_alloc_flags_, &page_list);
    if (allocated < count) {
        LTRACEF("failed to allocate enough pages (asked for %zu, got %zu)\n", count, allocated);
        pmm_free(&page_list);
        return ERR_NO_MEMORY;
    }

    // add them to the appropriate range of the object
    for (uint64_t o = offset; o < end; o += PAGE_SIZE) {
        vm_page_t* p = page_list_.GetPage(o);
        if (p)
            continue;

        p = list_remove_head_type(&page_list, vm_page_t, free.node);
        ASSERT(p);

        p->state = VM_PAGE_STATE_OBJECT;

        // TODO: remove once pmm returns zeroed pages
        ZeroPage(p);

        __UNUSED auto status = page_list_.AddPage(p, o);
        DEBUG_ASSERT(status == NO_ERROR);

        if (committed)
            *committed += PAGE_SIZE;
    }

    DEBUG_ASSERT(list_is_empty(&page_list));

    // for now we only support committing as much as we were asked for
    DEBUG_ASSERT(!committed || *committed == count * PAGE_SIZE);

    return NO_ERROR;
}

status_t VmObjectPaged::CommitRangeContiguous(uint64_t offset, uint64_t len, uint64_t* committed,
                                              uint8_t alignment_log2) {
    DEBUG_ASSERT(magic_ == MAGIC);
    LTRACEF("offset %#" PRIx64 ", len %#" PRIx64 ", alignment %hhu\n", offset, len, alignment_log2);

    if (committed)
        *committed = 0;

    AutoLock a(lock_);

    // trim the size
    if (!TrimRange(offset, len, size_))
        return ERR_OUT_OF_RANGE;

    // was in range, just zero length
    if (len == 0)
        return NO_ERROR;

    // compute a page aligned end to do our searches in to make sure we cover all the pages
    uint64_t end = ROUNDUP_PAGE_SIZE(offset + len);
    DEBUG_ASSERT(end > offset);

    // make a pass through the list, making sure we have an empty run on the object
    size_t count = 0;
    for (uint64_t o = offset; o < end; o += PAGE_SIZE) {
        if (!page_list_.GetPage(o))
            count++;
    }

    DEBUG_ASSERT(count == len / PAGE_SIZE);

    // allocate count number of pages
    list_node page_list;
    list_initialize(&page_list);

    size_t allocated = pmm_alloc_contiguous(count, pmm_alloc_flags_, alignment_log2, nullptr, &page_list);
    if (allocated < count) {
        LTRACEF("failed to allocate enough pages (asked for %zu, got %zu)\n", count, allocated);
        pmm_free(&page_list);
        return ERR_NO_MEMORY;
    }

    DEBUG_ASSERT(list_length(&page_list) == allocated);

    // add them to the appropriate range of the object
    for (uint64_t o = offset; o < end; o += PAGE_SIZE) {
        vm_page_t* p = list_remove_head_type(&page_list, vm_page_t, free.node);
        ASSERT(p);

        p->state = VM_PAGE_STATE_OBJECT;

        // TODO: remove once pmm returns zeroed pages
        ZeroPage(p);

        __UNUSED auto status = page_list_.AddPage(p, o);
        DEBUG_ASSERT(status == NO_ERROR);

        if (committed)
            *committed += PAGE_SIZE;
    }

    // for now we only support committing as much as we were asked for
    DEBUG_ASSERT(!committed || *committed == count * PAGE_SIZE);

    return NO_ERROR;
}

status_t VmObjectPaged::DecommitRange(uint64_t offset, uint64_t len, uint64_t* decommitted) {
    DEBUG_ASSERT(magic_ == MAGIC);
    LTRACEF("offset %#" PRIx64 ", len %#" PRIx64 "\n", offset, len);

    if (decommitted)
        *decommitted = 0;

    AutoLock a(lock_);

    // trim the size
    if (!TrimRange(offset, len, size_))
        return ERR_OUT_OF_RANGE;

    // was in range, just zero length
    if (len == 0)
        return NO_ERROR;

    // figure the starting and ending page offset
    uint64_t start = PAGE_ALIGN(offset);
    uint64_t end = ROUNDUP_PAGE_SIZE(offset + len);
    DEBUG_ASSERT(end > offset);
    DEBUG_ASSERT(end > start);
    uint64_t page_aligned_len = end - start;

    LTRACEF("start offset %#" PRIx64 ", end %#" PRIx64 ", page_aliged_len %#" PRIx64 "\n", start, end,
            page_aligned_len);

    // unmap all of the pages in this range on all the mapping regions
    for (auto& r : region_list_) {
        // unmap any pages the region may have mapped that intersect this range
        r.UnmapVmoRangeLocked(start, page_aligned_len);
    }

    // iterate through the pages, freeing them
    while (start < end) {
        auto status = page_list_.FreePage(start);
        if (status == NO_ERROR && decommitted) {
            *decommitted += PAGE_SIZE;
        }
        start += PAGE_SIZE;
    }

    return NO_ERROR;
}

status_t VmObjectPaged::Resize(uint64_t s) {
    DEBUG_ASSERT(magic_ == MAGIC);
    LTRACEF("vmo %p, size %" PRIu64 "\n", this, s);

    // there's a max size to keep indexes within range
    if (s > MAX_SIZE)
        return ERR_OUT_OF_RANGE;

    AutoLock a(lock_);

    // see if we're shrinking the vmo
    if (s < size_) {
        // figure the starting and ending page offset that is affected
        uint64_t start = ROUNDUP_PAGE_SIZE(s);
        uint64_t end = ROUNDUP_PAGE_SIZE(size_);
        uint64_t page_aligned_len = end - start;

        // we're only worried about whole pages to be removed
        if (page_aligned_len > 0) {
            // unmap all of the pages in this range on all the mapping regions
            for (auto& r : region_list_) {
                // unmap any pages the region may have mapped that intersect this range
                r.UnmapVmoRangeLocked(start, page_aligned_len);
            }

            // iterate through the pages, freeing them
            while (start < end) {
                page_list_.FreePage(start);
                start += PAGE_SIZE;
            }
        }
    }

    // save bytewise size
    size_ = s;

    return NO_ERROR;
}

// perform some sort of copy in/out on a range of the object using a passed in lambda
// for the copy routine
template <typename T>
status_t VmObjectPaged::ReadWriteInternal(uint64_t offset, size_t len, size_t* bytes_copied, bool write,
                                          T copyfunc) {
    DEBUG_ASSERT(magic_ == MAGIC);
    if (bytes_copied)
        *bytes_copied = 0;

    AutoLock a(lock_);

    // trim the size
    if (!TrimRange(offset, len, size_))
        return ERR_OUT_OF_RANGE;

    // was in range, just zero length
    if (len == 0)
        return 0;

    // walk the list of pages and do the write
    size_t dest_offset = 0;
    while (len > 0) {
        size_t page_offset = offset % PAGE_SIZE;
        size_t tocopy = MIN(PAGE_SIZE - page_offset, len);

        // fault in the page
        vm_page_t* p = FaultPageLocked(offset, write ? VMM_PF_FLAG_WRITE : 0);
        if (!p)
            return ERR_NO_MEMORY;

        // compute the kernel mapping of this page
        paddr_t pa = vm_page_to_paddr(p);
        uint8_t* page_ptr = reinterpret_cast<uint8_t*>(paddr_to_kvaddr(pa));

        // call the copy routine
        auto err = copyfunc(page_ptr + page_offset, dest_offset, tocopy);
        if (err < 0)
            return err;

        offset += tocopy;
        if (bytes_copied)
            *bytes_copied += tocopy;
        dest_offset += tocopy;
        len -= tocopy;
    }

    return NO_ERROR;
}

status_t VmObjectPaged::Read(void* _ptr, uint64_t offset, size_t len, size_t* bytes_read) {
    DEBUG_ASSERT(magic_ == MAGIC);
    // test to make sure this is a kernel pointer
    if (!is_kernel_address(reinterpret_cast<vaddr_t>(_ptr))) {
        DEBUG_ASSERT_MSG(0, "non kernel pointer passed\n");
        return ERR_INVALID_ARGS;
    }

    // read routine that just uses a memcpy
    uint8_t* ptr = reinterpret_cast<uint8_t*>(_ptr);
    auto read_routine = [ptr](const void* src, size_t offset, size_t len) -> status_t {
        memcpy(ptr + offset, src, len);
        return NO_ERROR;
    };

    return ReadWriteInternal(offset, len, bytes_read, false, read_routine);
}

status_t VmObjectPaged::Write(const void* _ptr, uint64_t offset, size_t len, size_t* bytes_written) {
    DEBUG_ASSERT(magic_ == MAGIC);
    // test to make sure this is a kernel pointer
    if (!is_kernel_address(reinterpret_cast<vaddr_t>(_ptr))) {
        DEBUG_ASSERT_MSG(0, "non kernel pointer passed\n");
        return ERR_INVALID_ARGS;
    }

    // write routine that just uses a memcpy
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(_ptr);
    auto write_routine = [ptr](void* dst, size_t offset, size_t len) -> status_t {
        memcpy(dst, ptr + offset, len);
        return NO_ERROR;
    };

    return ReadWriteInternal(offset, len, bytes_written, true, write_routine);
}

status_t VmObjectPaged::ReadUser(user_ptr<void> ptr, uint64_t offset, size_t len, size_t* bytes_read) {
    DEBUG_ASSERT(magic_ == MAGIC);

    // test to make sure this is a user pointer
    if (!ptr.is_user_address()) {
        return ERR_INVALID_ARGS;
    }

    // read routine that uses copy_to_user
    auto read_routine = [ptr](const void* src, size_t offset, size_t len) -> status_t {
        return ptr.byte_offset(offset).copy_array_to_user(src, len);
    };

    return ReadWriteInternal(offset, len, bytes_read, false, read_routine);
}

status_t VmObjectPaged::WriteUser(user_ptr<const void> ptr, uint64_t offset, size_t len,
                                  size_t* bytes_written) {
    DEBUG_ASSERT(magic_ == MAGIC);

    // test to make sure this is a user pointer
    if (!ptr.is_user_address()) {
        return ERR_INVALID_ARGS;
    }

    // write routine that uses copy_from_user
    auto write_routine = [ptr](void* dst, size_t offset, size_t len) -> status_t {
        return ptr.byte_offset(offset).copy_array_from_user(dst, len);
    };

    return ReadWriteInternal(offset, len, bytes_written, true, write_routine);
}

status_t VmObjectPaged::Lookup(uint64_t offset, uint64_t len, user_ptr<paddr_t> buffer, size_t buffer_size) {
    DEBUG_ASSERT(magic_ == MAGIC);

    if (unlikely(len == 0))
        return ERR_INVALID_ARGS;

    AutoLock a(lock_);

    // verify that the range is within the object
    if (unlikely(!InRange(offset, len, size_)))
        return ERR_OUT_OF_RANGE;

    uint64_t start_page_offset = ROUNDDOWN(offset, PAGE_SIZE);
    uint64_t end = offset + len;
    uint64_t end_page_offset = ROUNDUP(end, PAGE_SIZE);

    // compute the size of the table we'll need and make sure it fits in the user buffer
    uint64_t table_size = ((end_page_offset - start_page_offset) / PAGE_SIZE) * sizeof(paddr_t);
    if (unlikely(table_size > buffer_size))
        return ERR_BUFFER_TOO_SMALL;

    size_t index = 0;
    for (uint64_t off = start_page_offset; off != end_page_offset; off += PAGE_SIZE, index++) {
        // grab a pointer to the page only if it's already present
        vm_page_t* p = GetPageLocked(off);
        if (unlikely(!p))
            return ERR_NO_MEMORY;

        // find the physical address
        paddr_t pa = vm_page_to_paddr(p);

        // copy it out into user space
        auto status = buffer.element_offset(index).copy_to_user(pa);
        if (unlikely(status < 0))
            return status;
    }

    return NO_ERROR;
}

status_t VmObjectPaged::InvalidateCache(const uint64_t offset, const uint64_t len) {
    return CacheOp(offset, len, CacheOpType::Invalidate);
}

status_t VmObjectPaged::CleanCache(const uint64_t offset, const uint64_t len) {
    return CacheOp(offset, len, CacheOpType::Clean);
}

status_t VmObjectPaged::CleanInvalidateCache(const uint64_t offset, const uint64_t len) {
    return CacheOp(offset, len, CacheOpType::CleanInvalidate);
}

status_t VmObjectPaged::SyncCache(const uint64_t offset, const uint64_t len) {
    return CacheOp(offset, len, CacheOpType::Sync);
}

status_t VmObjectPaged::CacheOp(const uint64_t start_offset, const uint64_t len,
                                const CacheOpType type) {
    DEBUG_ASSERT(magic_ == MAGIC);

    if (unlikely(len == 0))
        return ERR_INVALID_ARGS;

    AutoLock a(lock_);

    if (unlikely(!InRange(start_offset, len, size_)))
        return ERR_OUT_OF_RANGE;

    const size_t end_offset = static_cast<size_t>(start_offset + len);
    size_t op_start_offset = static_cast<size_t>(start_offset);

    while (op_start_offset != end_offset) {
        // Offset at the end of the current page.
        const size_t page_end_offset = ROUNDUP(op_start_offset + 1, PAGE_SIZE);

        // This cache op will either terminate at the end of the current page or
        // at the end of the whole op range -- whichever comes first.
        const size_t op_end_offset = MIN(page_end_offset, end_offset);

        const size_t cache_op_len = op_end_offset - op_start_offset;

        const size_t page_offset = op_start_offset % PAGE_SIZE;

        vm_page_t* p = GetPageLocked(op_start_offset);

        if (likely(p)) {
            // Convert the page address to a Kernel virtual address.
            const paddr_t pa = vm_page_to_paddr(p);
            const void* ptr = paddr_to_kvaddr(pa);
            const addr_t cache_op_addr = reinterpret_cast<addr_t>(ptr) + page_offset;

            // Perform the necessary cache op against this page.
            switch(type) {
            case CacheOpType::Invalidate:
                arch_invalidate_cache_range(cache_op_addr, cache_op_len);
                break;
            case CacheOpType::Clean:
                arch_clean_cache_range(cache_op_addr, cache_op_len);
                break;
            case CacheOpType::CleanInvalidate:
                arch_clean_invalidate_cache_range(cache_op_addr, cache_op_len);
                break;
            case CacheOpType::Sync:
                arch_sync_cache_range(cache_op_addr, cache_op_len);
                break;
            }
        }

        op_start_offset += cache_op_len;
    }

    return NO_ERROR;
}