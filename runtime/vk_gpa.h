// The device address table (vk_gpa.c): GPU address pages to buffer device addresses for shaders' global memory accesses.
#pragma once
#include <stdint.h>

#include "vk.h"

#define VK_GPA_CB_SLOT 0x40   // res.json cb_slot of the table (translator ResourceManager.DeviceAddressTableCbSlot)

int vk_gpa_wanted(void);                   // the device has buffer device addresses and NX2NAVI_GPA is not 0
int vk_gpa_wanted_ctx(const VkCtx *c);     // the same during device creation, before vk_ctx() returns it
int vk_gpa_on(void);                       // the table exists (created on first use)
uint64_t vk_gpa_device_address(VkBuffer b);
VkDescriptorBufferInfo vk_gpa_info(void);  // the whole table, for a storage descriptor
// Maps the pages [lo, hi) holds whole to buf, whose byte 0 is GPU address buf_lo. Returns the record (0: not mapped).
uint32_t vk_gpa_map(uint64_t lo, uint64_t hi, VkBuffer buf, uint64_t buf_lo);
// Unmaps the pages of [lo, hi) that name rec; cover (may be NULL) gives the record of another live buffer that holds a
// page whole, or 0. rec is reused once the GPU finished the submissions issued until now.
void vk_gpa_unmap(uint64_t lo, uint64_t hi, uint32_t rec, uint32_t (*cover)(uint64_t page_addr));
// Unmaps every page of [lo, hi), whatever record it names (pages of a virtual pool whose physical memory goes away).
void vk_gpa_clear(uint64_t lo, uint64_t hi);
// Moves the pages of [lo, hi) that name old_rec (or none) to buf (a pool's VRAM mirror), with old_rec's buffer as their
// twin, which shaders write too; returns the new record.
uint32_t vk_gpa_remap(uint64_t lo, uint64_t hi, uint32_t old_rec, VkBuffer buf, uint64_t buf_lo);
// The same without the twin: the pages name buf alone (a pool whose VRAM mirror is evicted goes back to its imported
// buffer).
uint32_t vk_gpa_remap_plain(uint64_t lo, uint64_t hi, uint32_t old_rec, VkBuffer buf, uint64_t buf_lo);
// Replay: the pages shaders newly wrote through the table, as guest page addresses in *pages (valid until the next call);
// returns their number (vk_gpa.c "Written pages").
int vk_gpa_written_pages(const uint64_t **pages);
void vk_gpa_report(void);
void vk_gpa_shutdown(void);
