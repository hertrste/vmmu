#include "pt_walk.hpp"

#include <type_traits>

// TODO
// - PSE-36
// - PAE
// - Reserved bits checking

namespace {

// Page table level definitions

enum {
      IS_TERMINAL = 1 << 0,
      HAS_PS = 1 << 1,
      RESPECTS_CR4_PSE = 1 << 2,
};

template<typename WORD, typename TABLE_INDEX, typename NEXT_TABLE, typename FRAME_BITS, int FLAGS>
struct level {
  // Given a linear address return the index into this level of the page table.
  static uint64_t get_table_index(uint64_t linear_addr) { return TABLE_INDEX::extract(linear_addr); }

  // Given a page table entry of this level, return the base of the next level.
  static WORD get_next_table_base(WORD pte)
  {
    fast_assert(not (FLAGS & IS_TERMINAL));
    return NEXT_TABLE::extract_no_shift(pte);
  }

  static uint64_t get_page_frame(WORD pte)
  {
    return FRAME_BITS::extract_no_shift(pte);
  }

  static uint8_t get_page_frame_order()
  {
    return uint8_t(FRAME_BITS::lo);
  }

  static bool is_leaf(WORD pte, paging_state const &state)
  {
    if (FLAGS & IS_TERMINAL)
      return true;

    if (not (FLAGS & HAS_PS))
      return false;

    return (not (FLAGS & RESPECTS_CR4_PSE) or state.cr4_pse) and (pte & PTE_PS);
  }

  static bool has_reserved_bits_set(WORD pte, paging_state const &state)
  {
    // TODO Implement me
    return false;
  }

};

//                      WORD      INDEX              NEXT TABLE         FRAME              FLAGS
using pm32_pd   = level<uint32_t, bit_range<31, 22>, bit_range<31, 12>, bit_range<31, 22>, HAS_PS | RESPECTS_CR4_PSE>;
using pm32_pt   = level<uint32_t, bit_range<21, 12>, bit_range<31, 12>, bit_range<31, 12>, IS_TERMINAL>;

using pm64_pml4 = level<uint64_t, bit_range<47, 39>, bit_range<51, 12>, bit_range< 0,  0>, 0>;
using pm64_pdpt = level<uint64_t, bit_range<38, 30>, bit_range<51, 12>, bit_range<51, 30>, HAS_PS>;
using pm64_pd   = level<uint64_t, bit_range<29, 21>, bit_range<51, 12>, bit_range<51, 21>, HAS_PS>;
using pm64_pt   = level<uint64_t, bit_range<20, 12>, bit_range<51, 12>, bit_range<51, 12>, IS_TERMINAL>;

// Compute page fault information according to Intel SDM Vol 3 4.7 "Page-fault
// Exceptions".
page_fault_info get_pf_info(linear_memory_op const &op, paging_state const &state,
                            bool present, bool reserved_bits_set)
{
  uint32_t error = 0;

  if (present)
    error |= EC_P;

  if (op.is_write())
    error |= EC_W;

  if (not (op.is_implicit_supervisor() or state.is_supervisor()))
    error |= EC_U;

  if (present and reserved_bits_set)
    error |= EC_RSVD;

  if (op.is_instruction_fetch() and (state.cr4_smep or (state.cr4_pae and state.efer_nxe)))
    error |= EC_I;

  return { op.linear_addr, error };
}

// The main page table walking logic.
template <typename WORD, typename LEVEL, typename... REST>
translate_result walk(linear_memory_op const &op, paging_state const &state, abstract_memory *memory,
                      uint64_t table_base, tlb_attr attr = {})
{
  uint64_t const table_entry_addr = table_base + sizeof(WORD) * LEVEL::get_table_index(op.linear_addr);

  // TODO Get some sort of smart pointer back, so the memory backend can do
  // cmpxchg on normal memory without having to lookup the actual location
  // again.
  WORD table_entry = memory->read(table_entry_addr, WORD{});
  bool is_present = table_entry & PTE_P;
  bool is_rsvd = LEVEL::has_reserved_bits_set(table_entry, state);

  if (unlikely(not is_present or is_rsvd))
    return get_pf_info(op, state, is_present, is_rsvd);

  bool is_leaf = LEVEL::is_leaf(table_entry, state);

  // Set accessed bit
  if (unlikely(not (table_entry & PTE_A)) and
      not memory->cmpxchg(table_entry_addr, table_entry, table_entry | WORD(PTE_A)))
    return /* retry */ {};

  // Dirty flags only exist in leaf page table entries.
  attr = tlb_attr::combine(attr, tlb_attr {table_entry & (is_leaf ? 0 : ~PTE_D)});

  if (is_leaf) {
    uint64_t mask = (uint64_t(1) << LEVEL::get_page_frame_order()) - 1;
    auto tlbe = tlb_entry { op.linear_addr & ~mask, LEVEL::get_page_frame(table_entry),
                            LEVEL::get_page_frame_order(), attr };

    if (unlikely(not tlbe.allows(op, state)))
      return get_pf_info(op, state, true, false);

    // Update dirty bit, if not already set.
    //
    // TODO Optimize to use one cmpxchg in case both A/D bits need to be
    // updated. Otherwise, you can end up setting an A bit in a PTE that is not
    // actually used for translation.
    if (op.is_write() and unlikely(not attr.is_d())) {
      if (unlikely(not memory->cmpxchg(table_entry_addr, table_entry, table_entry | WORD(PTE_D))))
        return /* retry */ {};

      tlbe.attr.set_d();
    }

    return tlbe;
  } else {
    fast_assert(not is_leaf);

    // Continue page table walk with next level.
    if constexpr (sizeof...(REST) != 0)
                   return walk<WORD, REST...>(op, state, memory, LEVEL::get_next_table_base(table_entry), attr);

    __builtin_trap();
  }
}

// Special case of translate() for the PAE PDPTE lookup. We could possibly
// squeeze it in the above scheme, but it's easier to just spell out directly
// what happens for PDPTEs.
translate_result pae_walk(linear_memory_op const &op, paging_state const &state, abstract_memory *memory,
                          uint64_t table_base, tlb_attr attr = {})
{
  uint64_t pdpte = state.pdpte[bit_range<31, 30>::extract(op.linear_addr)];
  uint32_t next_table = bit_range<51, 12>::extract_no_shift(pdpte);

  if (not (pdpte & PTE_P))
    return get_pf_info(op, state, true, false);

  // Reserved bits cannot be set, because that would trigger a #GP on PDPTE
  // load.

  return walk<uint64_t, pm64_pd, pm64_pt>(op, state, memory, next_table);
}

} // namespace

translate_result translate(linear_memory_op const &op, paging_state const &state, abstract_memory *memory)
{
  tlb_attr attr;
  translate_result result;

  fast_assert(memory);

  do {
    switch (state.get_paging_mode()) {
    case paging_mode::PHYS:
      result = tlb_entry::no_paging();
      break;
    case paging_mode::PM32:
      result = walk<uint32_t, pm32_pd, pm32_pt>(op, state, memory, state.cr3 & 0xFFFFF000UL);
      break;
    case paging_mode::PM32_PAE:
      result = pae_walk(op, state, memory, state.cr3 & 0xFFFFF000UL);
      break;
    case paging_mode::PM64_4LEVEL:
      result = walk<uint64_t, pm64_pml4, pm64_pdpt, pm64_pd, pm64_pt>(op, state, memory, state.cr3 & ~0xFFFULL);
      break;
    default:
      __builtin_trap();
    }
  } while (std::holds_alternative<std::monostate>(result));

  return result;
}