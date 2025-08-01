// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "crimson/os/seastore/cache.h"

#include <sstream>
#include <string_view>

#include <seastar/core/metrics.hh>

#include "crimson/os/seastore/logging.h"
#include "crimson/common/config_proxy.h"
#include "crimson/os/seastore/async_cleaner.h"

// included for get_extent_by_type
#include "crimson/os/seastore/collection_manager/collection_flat_node.h"
#include "crimson/os/seastore/lba/lba_btree_node.h"
#include "crimson/os/seastore/omap_manager/btree/omap_btree_node_impl.h"
#include "crimson/os/seastore/object_data_handler.h"
#include "crimson/os/seastore/collection_manager/collection_flat_node.h"
#include "crimson/os/seastore/onode_manager/staged-fltree/node_extent_manager/seastore.h"
#include "crimson/os/seastore/backref/backref_tree_node.h"
#include "test/crimson/seastore/test_block.h"

using std::string_view;

SET_SUBSYS(seastore_cache);

namespace crimson::os::seastore {

Cache::Cache(
  ExtentPlacementManager &epm)
  : epm(epm),
    pinboard(create_extent_pinboard(
      crimson::common::get_conf<Option::size_t>(
       "seastore_cachepin_size_pershard")))
{
  register_metrics();
  segment_providers_by_device_id.resize(DEVICE_ID_MAX, nullptr);
}

Cache::~Cache()
{
  LOG_PREFIX(Cache::~Cache);
  for (auto &i: extents_index) {
    ERROR("extent is still alive -- {}", i);
  }
  ceph_assert(extents_index.empty());
}

// TODO: this method can probably be removed in the future
Cache::retire_extent_ret Cache::retire_extent_addr(
  Transaction &t, paddr_t paddr, extent_len_t length)
{
  LOG_PREFIX(Cache::retire_extent_addr);
  TRACET("retire {}~0x{:x}", t, paddr, length);

  assert(paddr.is_real_location());

  CachedExtentRef ext;
  auto result = t.get_extent(paddr, &ext);
  if (result == Transaction::get_extent_ret::PRESENT) {
    DEBUGT("retire {}~0x{:x} on t -- {}",
           t, paddr, length, *ext);
    t.add_present_to_retired_set(ext);
    return retire_extent_iertr::now();
  } else if (result == Transaction::get_extent_ret::RETIRED) {
    ERRORT("retire {}~0x{:x} failed, already retired -- {}",
           t, paddr, length, *ext);
    ceph_abort();
  }

  // any record-relative or delayed paddr must have been on the transaction
  assert(paddr.is_absolute());

  // absent from transaction
  // retiring is not included by the cache hit metrics
  ext = query_cache(paddr);
  if (ext) {
    DEBUGT("retire {}~0x{:x} in cache -- {}", t, paddr, length, *ext);
  } else {
    // add a new placeholder to Cache
    ext = CachedExtent::make_cached_extent_ref<
      RetiredExtentPlaceholder>(length);
    ext->init(
      CachedExtent::extent_state_t::CLEAN, paddr,
      PLACEMENT_HINT_NULL, NULL_GENERATION, TRANS_ID_NULL);
    DEBUGT("retire {}~0x{:x} as placeholder, add extent -- {}",
           t, paddr, length, *ext);
    add_extent(ext);
  }
  t.add_absent_to_retired_set(ext);
  return retire_extent_iertr::now();
}

void Cache::retire_absent_extent_addr(
  Transaction &t, paddr_t paddr, extent_len_t length)
{
  assert(paddr.is_absolute());

  CachedExtentRef ext;
#ifndef NDEBUG
  auto result = t.get_extent(paddr, &ext);
  assert(result != Transaction::get_extent_ret::PRESENT
    && result != Transaction::get_extent_ret::RETIRED);
  assert(!query_cache(paddr));
#endif
  LOG_PREFIX(Cache::retire_absent_extent_addr);
  // add a new placeholder to Cache
  ext = CachedExtent::make_cached_extent_ref<
    RetiredExtentPlaceholder>(length);
  ext->init(
    CachedExtent::extent_state_t::CLEAN, paddr,
    PLACEMENT_HINT_NULL, NULL_GENERATION, TRANS_ID_NULL);
  DEBUGT("retire {}~0x{:x} as placeholder, add extent -- {}",
	 t, paddr, length, *ext);
  add_extent(ext);
  t.add_absent_to_retired_set(ext);
}

void Cache::dump_contents()
{
  LOG_PREFIX(Cache::dump_contents);
  DEBUG("enter");
  for (auto &&i: extents_index) {
    DEBUG("live {}", i);
  }
  DEBUG("exit");
}

void Cache::register_metrics()
{
  LOG_PREFIX(Cache::register_metrics);
  DEBUG("");

  stats = {};
  last_dirty_io = {};
  last_dirty_io_by_src_ext = {};
  last_trim_rewrites = {};
  last_reclaim_rewrites = {};
  last_access = {};
  last_cache_absent_by_src = {};
  last_access_by_src_ext = {};

  namespace sm = seastar::metrics;
  using src_t = Transaction::src_t;

  std::map<src_t, sm::label_instance> labels_by_src {
    {src_t::MUTATE, sm::label_instance("src", "MUTATE")},
    {src_t::READ, sm::label_instance("src", "READ")},
    {src_t::TRIM_DIRTY, sm::label_instance("src", "TRIM_DIRTY")},
    {src_t::TRIM_ALLOC, sm::label_instance("src", "TRIM_ALLOC")},
    {src_t::CLEANER_MAIN, sm::label_instance("src", "CLEANER_MAIN")},
    {src_t::CLEANER_COLD, sm::label_instance("src", "CLEANER_COLD")},
  };
  assert(labels_by_src.size() == (std::size_t)src_t::MAX);

  std::map<extent_types_t, sm::label_instance> labels_by_ext {
    {extent_types_t::ROOT,                sm::label_instance("ext", "ROOT")},
    {extent_types_t::LADDR_INTERNAL,      sm::label_instance("ext", "LADDR_INTERNAL")},
    {extent_types_t::LADDR_LEAF,          sm::label_instance("ext", "LADDR_LEAF")},
    {extent_types_t::DINK_LADDR_LEAF,     sm::label_instance("ext", "DINK_LADDR_LEAF")},
    {extent_types_t::ROOT_META,           sm::label_instance("ext", "ROOT_META")},
    {extent_types_t::OMAP_INNER,          sm::label_instance("ext", "OMAP_INNER")},
    {extent_types_t::OMAP_LEAF,           sm::label_instance("ext", "OMAP_LEAF")},
    {extent_types_t::ONODE_BLOCK_STAGED,  sm::label_instance("ext", "ONODE_BLOCK_STAGED")},
    {extent_types_t::COLL_BLOCK,          sm::label_instance("ext", "COLL_BLOCK")},
    {extent_types_t::OBJECT_DATA_BLOCK,   sm::label_instance("ext", "OBJECT_DATA_BLOCK")},
    {extent_types_t::RETIRED_PLACEHOLDER, sm::label_instance("ext", "RETIRED_PLACEHOLDER")},
    {extent_types_t::ALLOC_INFO,      	  sm::label_instance("ext", "ALLOC_INFO")},
    {extent_types_t::JOURNAL_TAIL,        sm::label_instance("ext", "JOURNAL_TAIL")},
    {extent_types_t::TEST_BLOCK,          sm::label_instance("ext", "TEST_BLOCK")},
    {extent_types_t::TEST_BLOCK_PHYSICAL, sm::label_instance("ext", "TEST_BLOCK_PHYSICAL")},
    {extent_types_t::BACKREF_INTERNAL,    sm::label_instance("ext", "BACKREF_INTERNAL")},
    {extent_types_t::BACKREF_LEAF,        sm::label_instance("ext", "BACKREF_LEAF")}
  };
  assert(labels_by_ext.size() == (std::size_t)extent_types_t::NONE);

  /*
   * trans_created
   */
  for (auto& [src, src_label] : labels_by_src) {
    metrics.add_group(
      "cache",
      {
        sm::make_counter(
          "trans_created",
          get_by_src(stats.trans_created_by_src, src),
          sm::description("total number of transaction created"),
          {src_label}
        ),
      }
    );
  }

  /*
   * cache_query: cache_access and cache_hit
   */
  metrics.add_group(
    "cache",
    {
      sm::make_counter(
        "cache_access",
        [this] {
          return stats.access.get_cache_access();
        },
        sm::description("total number of cache accesses")
      ),
      sm::make_counter(
        "cache_hit",
        [this] {
          return stats.access.get_cache_hit();
        },
        sm::description("total number of cache hits")
      ),
    }
  );

  {
    /*
     * efforts discarded/committed
     */
    auto effort_label = sm::label("effort");

    // invalidated efforts
    using namespace std::literals::string_view_literals;
    const string_view invalidated_effort_names[] = {
      "READ"sv,
      "MUTATE"sv,
      "RETIRE"sv,
      "FRESH"sv,
      "FRESH_OOL_WRITTEN"sv,
    };
    for (auto& [src, src_label] : labels_by_src) {
      auto& efforts = get_by_src(stats.invalidated_efforts_by_src, src);
      for (auto& [ext, ext_label] : labels_by_ext) {
        auto& counter = get_by_ext(efforts.num_trans_invalidated, ext);
        metrics.add_group(
          "cache",
          {
            sm::make_counter(
              "trans_invalidated_by_extent",
              counter,
              sm::description("total number of transactions invalidated by extents"),
              {src_label, ext_label}
            ),
          }
        );
      }

      if (src == src_t::READ) {
        // read transaction won't have non-read efforts
        auto read_effort_label = effort_label("READ");
        metrics.add_group(
          "cache",
          {
            sm::make_counter(
              "invalidated_extents",
              efforts.read.num,
              sm::description("extents of invalidated transactions"),
              {src_label, read_effort_label}
            ),
            sm::make_counter(
              "invalidated_extent_bytes",
              efforts.read.bytes,
              sm::description("extent bytes of invalidated transactions"),
              {src_label, read_effort_label}
            ),
          }
        );
        continue;
      }

      // non READ invalidated efforts
      for (auto& effort_name : invalidated_effort_names) {
        auto& effort = [&effort_name, &efforts]() -> io_stat_t& {
          if (effort_name == "READ") {
            return efforts.read;
          } else if (effort_name == "MUTATE") {
            return efforts.mutate;
          } else if (effort_name == "RETIRE") {
            return efforts.retire;
          } else if (effort_name == "FRESH") {
            return efforts.fresh;
          } else {
            assert(effort_name == "FRESH_OOL_WRITTEN");
            return efforts.fresh_ool_written;
          }
        }();
        metrics.add_group(
          "cache",
          {
            sm::make_counter(
              "invalidated_extents",
              effort.num,
              sm::description("extents of invalidated transactions"),
              {src_label, effort_label(effort_name)}
            ),
            sm::make_counter(
              "invalidated_extent_bytes",
              effort.bytes,
              sm::description("extent bytes of invalidated transactions"),
              {src_label, effort_label(effort_name)}
            ),
          }
        );
      } // effort_name

      metrics.add_group(
        "cache",
        {
          sm::make_counter(
            "trans_invalidated",
            efforts.total_trans_invalidated,
            sm::description("total number of transactions invalidated"),
            {src_label}
          ),
          sm::make_counter(
            "invalidated_delta_bytes",
            efforts.mutate_delta_bytes,
            sm::description("delta bytes of invalidated transactions"),
            {src_label}
          ),
          sm::make_counter(
            "invalidated_ool_records",
            efforts.num_ool_records,
            sm::description("number of ool-records from invalidated transactions"),
            {src_label}
          ),
          sm::make_counter(
            "invalidated_ool_record_bytes",
            efforts.ool_record_bytes,
            sm::description("bytes of ool-record from invalidated transactions"),
            {src_label}
          ),
        }
      );
    } // src

    // committed efforts
    const string_view committed_effort_names[] = {
      "READ"sv,
      "MUTATE"sv,
      "RETIRE"sv,
      "FRESH_INVALID"sv,
      "FRESH_INLINE"sv,
      "FRESH_OOL"sv,
    };
    for (auto& [src, src_label] : labels_by_src) {
      if (src == src_t::READ) {
        // READ transaction won't commit
        continue;
      }
      auto& efforts = get_by_src(stats.committed_efforts_by_src, src);
      metrics.add_group(
        "cache",
        {
          sm::make_counter(
            "trans_committed",
            efforts.num_trans,
            sm::description("total number of transaction committed"),
            {src_label}
          ),
          sm::make_counter(
            "committed_ool_records",
            efforts.num_ool_records,
            sm::description("number of ool-records from committed transactions"),
            {src_label}
          ),
          sm::make_counter(
            "committed_ool_record_metadata_bytes",
            efforts.ool_record_metadata_bytes,
            sm::description("bytes of ool-record metadata from committed transactions"),
            {src_label}
          ),
          sm::make_counter(
            "committed_ool_record_data_bytes",
            efforts.ool_record_data_bytes,
            sm::description("bytes of ool-record data from committed transactions"),
            {src_label}
          ),
          sm::make_counter(
            "committed_inline_record_metadata_bytes",
            efforts.inline_record_metadata_bytes,
            sm::description("bytes of inline-record metadata from committed transactions"
                            "(excludes delta buffer)"),
            {src_label}
          ),
        }
      );
      for (auto& effort_name : committed_effort_names) {
        auto& effort_by_ext = [&efforts, &effort_name]()
            -> counter_by_extent_t<io_stat_t>& {
          if (effort_name == "READ") {
            return efforts.read_by_ext;
          } else if (effort_name == "MUTATE") {
            return efforts.mutate_by_ext;
          } else if (effort_name == "RETIRE") {
            return efforts.retire_by_ext;
          } else if (effort_name == "FRESH_INVALID") {
            return efforts.fresh_invalid_by_ext;
          } else if (effort_name == "FRESH_INLINE") {
            return efforts.fresh_inline_by_ext;
          } else {
            assert(effort_name == "FRESH_OOL");
            return efforts.fresh_ool_by_ext;
          }
        }();
        for (auto& [ext, ext_label] : labels_by_ext) {
          auto& effort = get_by_ext(effort_by_ext, ext);
          metrics.add_group(
            "cache",
            {
              sm::make_counter(
                "committed_extents",
                effort.num,
                sm::description("extents of committed transactions"),
                {src_label, effort_label(effort_name), ext_label}
              ),
              sm::make_counter(
                "committed_extent_bytes",
                effort.bytes,
                sm::description("extent bytes of committed transactions"),
                {src_label, effort_label(effort_name), ext_label}
              ),
            }
          );
        } // ext
      } // effort_name

      auto& delta_by_ext = efforts.delta_bytes_by_ext;
      for (auto& [ext, ext_label] : labels_by_ext) {
        auto& value = get_by_ext(delta_by_ext, ext);
        metrics.add_group(
          "cache",
          {
            sm::make_counter(
              "committed_delta_bytes",
              value,
              sm::description("delta bytes of committed transactions"),
              {src_label, ext_label}
            ),
          }
        );
      } // ext
    } // src

    // successful read efforts
    metrics.add_group(
      "cache",
      {
        sm::make_counter(
          "trans_read_successful",
          stats.success_read_efforts.num_trans,
          sm::description("total number of successful read transactions")
        ),
        sm::make_counter(
          "successful_read_extents",
          stats.success_read_efforts.read.num,
          sm::description("extents of successful read transactions")
        ),
        sm::make_counter(
          "successful_read_extent_bytes",
          stats.success_read_efforts.read.bytes,
          sm::description("extent bytes of successful read transactions")
        ),
      }
    );
  }

  /**
   * Cached extents (including placeholders)
   *
   * Dirty extents
   */
  metrics.add_group(
    "cache",
    {
      sm::make_counter(
        "cached_extents",
        [this] {
          return extents_index.size();
        },
        sm::description("total number of cached extents")
      ),
      sm::make_counter(
        "cached_extent_bytes",
        [this] {
          return extents_index.get_bytes();
        },
        sm::description("total bytes of cached extents")
      ),
      sm::make_counter(
        "dirty_extents",
        [this] {
          return dirty.size();
        },
        sm::description("total number of dirty extents")
      ),
      sm::make_counter(
        "dirty_extent_bytes",
        stats.dirty_bytes,
        sm::description("total bytes of dirty extents")
      ),
    }
  );

  pinboard->register_metrics();

  /**
   * tree stats
   */
  auto tree_label = sm::label("tree");
  auto onode_label = tree_label("ONODE");
  auto omap_label = tree_label("OMAP");
  auto lba_label = tree_label("LBA");
  auto backref_label = tree_label("BACKREF");
  auto register_tree_metrics = [&labels_by_src, &onode_label, &omap_label, this](
      const sm::label_instance& tree_label,
      uint64_t& tree_depth,
      int64_t& tree_extents_num,
      counter_by_src_t<tree_efforts_t>& committed_tree_efforts,
      counter_by_src_t<tree_efforts_t>& invalidated_tree_efforts) {
    metrics.add_group(
      "cache",
      {
        sm::make_counter(
          "tree_depth",
          tree_depth,
          sm::description("the depth of tree"),
          {tree_label}
        ),
	sm::make_counter(
	  "tree_extents_num",
	  tree_extents_num,
	  sm::description("num of extents of the tree"),
	  {tree_label}
	)
      }
    );
    for (auto& [src, src_label] : labels_by_src) {
      if (src == src_t::READ) {
        // READ transaction won't contain any tree inserts and erases
        continue;
      }
      if (is_background_transaction(src) &&
          (tree_label == onode_label ||
           tree_label == omap_label)) {
        // CLEANER transaction won't contain any onode/omap tree operations
        continue;
      }
      auto& committed_efforts = get_by_src(committed_tree_efforts, src);
      auto& invalidated_efforts = get_by_src(invalidated_tree_efforts, src);
      metrics.add_group(
        "cache",
        {
          sm::make_counter(
            "tree_inserts_committed",
            committed_efforts.num_inserts,
            sm::description("total number of committed insert operations"),
            {tree_label, src_label}
          ),
          sm::make_counter(
            "tree_erases_committed",
            committed_efforts.num_erases,
            sm::description("total number of committed erase operations"),
            {tree_label, src_label}
          ),
          sm::make_counter(
            "tree_updates_committed",
            committed_efforts.num_updates,
            sm::description("total number of committed update operations"),
            {tree_label, src_label}
          ),
          sm::make_counter(
            "tree_inserts_invalidated",
            invalidated_efforts.num_inserts,
            sm::description("total number of invalidated insert operations"),
            {tree_label, src_label}
          ),
          sm::make_counter(
            "tree_erases_invalidated",
            invalidated_efforts.num_erases,
            sm::description("total number of invalidated erase operations"),
            {tree_label, src_label}
          ),
          sm::make_counter(
            "tree_updates_invalidated",
            invalidated_efforts.num_updates,
            sm::description("total number of invalidated update operations"),
            {tree_label, src_label}
          ),
        }
      );
    }
  };
  register_tree_metrics(
      onode_label,
      stats.onode_tree_depth,
      stats.onode_tree_extents_num,
      stats.committed_onode_tree_efforts,
      stats.invalidated_onode_tree_efforts);
  register_tree_metrics(
      omap_label,
      stats.omap_tree_depth,
      stats.omap_tree_extents_num,
      stats.committed_omap_tree_efforts,
      stats.invalidated_omap_tree_efforts);
  register_tree_metrics(
      lba_label,
      stats.lba_tree_depth,
      stats.lba_tree_extents_num,
      stats.committed_lba_tree_efforts,
      stats.invalidated_lba_tree_efforts);
  register_tree_metrics(
      backref_label,
      stats.backref_tree_depth,
      stats.backref_tree_extents_num,
      stats.committed_backref_tree_efforts,
      stats.invalidated_backref_tree_efforts);

  /**
   * conflict combinations
   */
  auto srcs_label = sm::label("srcs");
  auto num_srcs = static_cast<std::size_t>(Transaction::src_t::MAX);
  std::size_t srcs_index = 0;
  for (uint8_t src2_int = 0; src2_int < num_srcs; ++src2_int) {
    auto src2 = static_cast<Transaction::src_t>(src2_int);
    for (uint8_t src1_int = src2_int; src1_int < num_srcs; ++src1_int) {
      ++srcs_index;
      auto src1 = static_cast<Transaction::src_t>(src1_int);
      // impossible combinations
      // should be consistent with checks in account_conflict()
      if ((src1 == Transaction::src_t::READ &&
           src2 == Transaction::src_t::READ) ||
          (src1 == Transaction::src_t::TRIM_DIRTY &&
           src2 == Transaction::src_t::TRIM_DIRTY) ||
          (src1 == Transaction::src_t::CLEANER_MAIN &&
           src2 == Transaction::src_t::CLEANER_MAIN) ||
          (src1 == Transaction::src_t::CLEANER_COLD &&
           src2 == Transaction::src_t::CLEANER_COLD) ||
          (src1 == Transaction::src_t::TRIM_ALLOC &&
           src2 == Transaction::src_t::TRIM_ALLOC)) {
        continue;
      }
      std::ostringstream oss;
      oss << src1 << "," << src2;
      metrics.add_group(
        "cache",
        {
          sm::make_counter(
            "trans_srcs_invalidated",
            stats.trans_conflicts_by_srcs[srcs_index - 1],
            sm::description("total number conflicted transactions by src pair"),
            {srcs_label(oss.str())}
          ),
        }
      );
    }
  }
  assert(srcs_index == NUM_SRC_COMB);
  srcs_index = 0;
  for (uint8_t src_int = 0; src_int < num_srcs; ++src_int) {
    ++srcs_index;
    auto src = static_cast<Transaction::src_t>(src_int);
    std::ostringstream oss;
    oss << "UNKNOWN," << src;
    metrics.add_group(
      "cache",
      {
        sm::make_counter(
          "trans_srcs_invalidated",
          stats.trans_conflicts_by_unknown[srcs_index - 1],
          sm::description("total number conflicted transactions by src pair"),
          {srcs_label(oss.str())}
        ),
      }
    );
  }

  /**
   * rewrite version
   */
  metrics.add_group(
    "cache",
    {
      sm::make_counter(
        "version_count_dirty",
        [this] {
          return stats.trim_rewrites.get_num_rewrites();
        },
        sm::description("total number of rewrite-dirty extents")
      ),
      sm::make_counter(
        "version_sum_dirty",
        stats.trim_rewrites.dirty_version,
        sm::description("sum of the version from rewrite-dirty extents")
      ),
      sm::make_counter(
        "version_count_reclaim",
        [this] {
          return stats.reclaim_rewrites.get_num_rewrites();
        },
        sm::description("total number of rewrite-reclaim extents")
      ),
      sm::make_counter(
        "version_sum_reclaim",
        stats.reclaim_rewrites.dirty_version,
        sm::description("sum of the version from rewrite-reclaim extents")
      ),
    }
  );
}

void Cache::add_extent(CachedExtentRef ref)
{
  assert(ref->is_valid());
  assert(ref->user_hint == PLACEMENT_HINT_NULL);
  assert(ref->rewrite_generation == NULL_GENERATION);
  assert(ref->get_paddr().is_absolute() ||
         ref->get_paddr().is_root());
  extents_index.insert(*ref);
}

void Cache::mark_dirty(CachedExtentRef ref)
{
  assert(ref->get_paddr().is_absolute());
  if (ref->is_stable_dirty()) {
    assert(ref->is_linked_to_list());
    return;
  }

  pinboard->remove(*ref);
  ref->state = CachedExtent::extent_state_t::DIRTY;
  add_to_dirty(ref, nullptr);
}

void Cache::add_to_dirty(
    CachedExtentRef ref,
    const Transaction::src_t* p_src)
{
  assert(ref->is_stable_dirty());
  assert(!ref->is_linked_to_list());
  ceph_assert(ref->get_modify_time() != NULL_TIME);
  assert(ref->is_fully_loaded());
  assert(ref->get_paddr().is_absolute() ||
         ref->get_paddr().is_root());

  intrusive_ptr_add_ref(&*ref);
  dirty.push_back(*ref);

  auto extent_length = ref->get_length();
  stats.dirty_bytes += extent_length;
  get_by_ext(
    stats.dirty_sizes_by_ext,
    ref->get_type()
  ).account_in(extent_length);
  if (p_src != nullptr) {
    assert(!is_root_type(ref->get_type()));
    stats.dirty_io.in_sizes.account_in(extent_length);
    get_by_ext(
      get_by_src(stats.dirty_io_by_src_ext, *p_src),
      ref->get_type()
    ).in_sizes.account_in(extent_length);
  }
}

void Cache::remove_from_dirty(
    CachedExtentRef ref,
    const Transaction::src_t* p_src)
{
  assert(ref->is_stable_dirty());
  ceph_assert(ref->is_linked_to_list());
  assert(ref->is_fully_loaded());
  assert(ref->get_paddr().is_absolute() ||
         ref->get_paddr().is_root());

  auto extent_length = ref->get_length();
  stats.dirty_bytes -= extent_length;
  get_by_ext(
    stats.dirty_sizes_by_ext,
    ref->get_type()
  ).account_out(extent_length);
  if (p_src != nullptr) {
    assert(!is_root_type(ref->get_type()));
    stats.dirty_io.out_sizes.account_in(extent_length);
    stats.dirty_io.out_versions += ref->get_version();
    auto& dirty_stats = get_by_ext(
      get_by_src(stats.dirty_io_by_src_ext, *p_src),
      ref->get_type());
    dirty_stats.out_sizes.account_in(extent_length);
    dirty_stats.out_versions += ref->get_version();
  }

  dirty.erase(dirty.s_iterator_to(*ref));
  intrusive_ptr_release(&*ref);
}

void Cache::replace_dirty(
    CachedExtentRef next,
    CachedExtentRef prev,
    const Transaction::src_t& src)
{
  assert(prev->is_stable_dirty());
  ceph_assert(prev->is_linked_to_list());
  assert(prev->is_fully_loaded());

  assert(next->is_stable_dirty());
  assert(!next->is_linked_to_list());
  ceph_assert(next->get_modify_time() != NULL_TIME);
  assert(next->is_fully_loaded());

  assert(prev->get_dirty_from() == next->get_dirty_from());
  assert(prev->get_length() == next->get_length());
  assert(!is_root_type(next->get_type()));
  assert(prev->get_type() == next->get_type());

  stats.dirty_io.num_replace += 1;
  get_by_ext(
    get_by_src(stats.dirty_io_by_src_ext, src),
    next->get_type()).num_replace += 1;

  auto prev_it = dirty.iterator_to(*prev);
  dirty.insert(prev_it, *next);
  dirty.erase(prev_it);
  intrusive_ptr_release(&*prev);
  intrusive_ptr_add_ref(&*next);
}

void Cache::clear_dirty()
{
  for (auto i = dirty.begin(); i != dirty.end(); ) {
    auto ptr = &*i;
    assert(ptr->is_stable_dirty());
    ceph_assert(ptr->is_linked_to_list());
    assert(ptr->is_fully_loaded());

    auto extent_length = ptr->get_length();
    stats.dirty_bytes -= extent_length;
    get_by_ext(
      stats.dirty_sizes_by_ext,
      ptr->get_type()
    ).account_out(extent_length);

    dirty.erase(i++);
    intrusive_ptr_release(ptr);
  }
  assert(stats.dirty_bytes == 0);
}

void Cache::remove_extent(
    CachedExtentRef ref,
    const Transaction::src_t* p_src)
{
  assert(ref->is_valid());
  assert(ref->get_paddr().is_absolute() ||
         ref->get_paddr().is_root());
  if (ref->is_stable_dirty()) {
    remove_from_dirty(ref, p_src);
  } else if (!ref->is_placeholder()) {
    assert(ref->get_paddr().is_absolute());
    pinboard->remove(*ref);
  }
  extents_index.erase(*ref);
}

void Cache::commit_retire_extent(
    Transaction& t,
    CachedExtentRef ref)
{
  const auto t_src = t.get_src();
  remove_extent(ref, &t_src);

  ref->dirty_from = JOURNAL_SEQ_NULL;
  invalidate_extent(t, *ref);
}

void Cache::commit_replace_extent(
    Transaction& t,
    CachedExtentRef next,
    CachedExtentRef prev)
{
  assert(next->get_paddr() == prev->get_paddr());
  assert(next->get_paddr().is_absolute() || next->get_paddr().is_root());
  assert(next->version == prev->version + 1);
  extents_index.replace(*next, *prev);

  const auto t_src = t.get_src();
  if (is_root_type(prev->get_type())) {
    assert(prev->is_stable_dirty());
    assert(prev->is_linked_to_list());
    // add the new dirty root to front
    remove_from_dirty(prev, nullptr/* exclude root */);
    add_to_dirty(next, nullptr/* exclude root */);
  } else if (prev->is_stable_dirty()) {
    replace_dirty(next, prev, t_src);
  } else {
    pinboard->remove(*prev);
    add_to_dirty(next, &t_src);
  }

  invalidate_extent(t, *prev);
}

void Cache::invalidate_extent(
    Transaction& t,
    CachedExtent& extent)
{
  if (!extent.may_conflict()) {
    assert(extent.read_transactions.empty());
    extent.set_invalid(t);
    return;
  }

  LOG_PREFIX(Cache::invalidate_extent);
  bool do_conflict_log = true;
  for (auto &&i: extent.read_transactions) {
    if (!i.t->conflicted) {
      if (do_conflict_log) {
        SUBDEBUGT(seastore_t, "conflict begin -- {}", t, extent);
        do_conflict_log = false;
      }
      assert(!i.t->is_weak());
      account_conflict(t.get_src(), i.t->get_src());
      mark_transaction_conflicted(*i.t, extent);
    }
  }
  extent.set_invalid(t);
}

void Cache::mark_transaction_conflicted(
  Transaction& t, CachedExtent& conflicting_extent)
{
  LOG_PREFIX(Cache::mark_transaction_conflicted);
  SUBTRACET(seastore_t, "", t);
  assert(!t.conflicted);
  t.conflicted = true;

  auto& efforts = get_by_src(stats.invalidated_efforts_by_src,
                             t.get_src());
  ++efforts.total_trans_invalidated;

  auto& counter = get_by_ext(efforts.num_trans_invalidated,
                             conflicting_extent.get_type());
  ++counter;

  io_stat_t read_stat;
  for (auto &i: t.read_set) {
    read_stat.increment(i.ref->get_length());
  }
  efforts.read.increment_stat(read_stat);

  if (t.get_src() != Transaction::src_t::READ) {
    io_stat_t retire_stat;
    for (auto &i: t.retired_set) {
      retire_stat.increment(i.extent->get_length());
    }
    efforts.retire.increment_stat(retire_stat);

    auto& fresh_stat = t.get_fresh_block_stats();
    efforts.fresh.increment_stat(fresh_stat);

    io_stat_t delta_stat;
    for (auto &i: t.mutated_block_list) {
      if (!i->is_valid()) {
        continue;
      }
      efforts.mutate.increment(i->get_length());
      delta_stat.increment(i->get_delta().length());
    }
    efforts.mutate_delta_bytes += delta_stat.bytes;

    if (t.get_pending_ool()) {
      t.get_pending_ool()->is_conflicted = true;
    } else {
      for (auto &i: t.pre_alloc_list) {
	epm.mark_space_free(i->get_paddr(), i->get_length());
      }
    }

    auto& ool_stats = t.get_ool_write_stats();
    efforts.fresh_ool_written.increment_stat(ool_stats.extents);
    efforts.num_ool_records += ool_stats.num_records;
    auto ool_record_bytes = (ool_stats.md_bytes + ool_stats.get_data_bytes());
    efforts.ool_record_bytes += ool_record_bytes;

    if (is_background_transaction(t.get_src())) {
      // CLEANER transaction won't contain any onode/omap tree operations
      assert(t.onode_tree_stats.is_clear());
      assert(t.omap_tree_stats.is_clear());
    } else {
      get_by_src(stats.invalidated_onode_tree_efforts, t.get_src()
          ).increment(t.onode_tree_stats);
      get_by_src(stats.invalidated_omap_tree_efforts, t.get_src()
          ).increment(t.omap_tree_stats);
    }

    get_by_src(stats.invalidated_lba_tree_efforts, t.get_src()
        ).increment(t.lba_tree_stats);
    get_by_src(stats.invalidated_backref_tree_efforts, t.get_src()
        ).increment(t.backref_tree_stats);

    SUBDEBUGT(seastore_t,
        "discard {} read, {} fresh, {} delta, {} retire, {}({}B) ool-records",
        t,
        read_stat,
        fresh_stat,
        delta_stat,
        retire_stat,
        ool_stats.num_records,
        ool_record_bytes);
  } else {
    // read transaction won't have non-read efforts
    assert(t.retired_set.empty());
    assert(t.get_fresh_block_stats().is_clear());
    assert(t.mutated_block_list.empty());
    assert(t.get_ool_write_stats().is_clear());
    assert(t.onode_tree_stats.is_clear());
    assert(t.omap_tree_stats.is_clear());
    assert(t.lba_tree_stats.is_clear());
    assert(t.backref_tree_stats.is_clear());
    SUBDEBUGT(seastore_t, "discard {} read", t, read_stat);
  }
}

void Cache::on_transaction_destruct(Transaction& t)
{
  LOG_PREFIX(Cache::on_transaction_destruct);
  SUBTRACET(seastore_t, "", t);
  if (t.get_src() == Transaction::src_t::READ &&
      t.conflicted == false) {
    io_stat_t read_stat;
    for (auto &i: t.read_set) {
      read_stat.increment(i.ref->get_length());
    }
    SUBDEBUGT(seastore_t, "done {} read", t, read_stat);

    if (!t.is_weak()) {
      // exclude weak transaction as it is impossible to conflict
      ++stats.success_read_efforts.num_trans;
      stats.success_read_efforts.read.increment_stat(read_stat);
    }

    // read transaction won't have non-read efforts
    assert(t.retired_set.empty());
    assert(t.get_fresh_block_stats().is_clear());
    assert(t.mutated_block_list.empty());
    assert(t.onode_tree_stats.is_clear());
    assert(t.omap_tree_stats.is_clear());
    assert(t.lba_tree_stats.is_clear());
    assert(t.backref_tree_stats.is_clear());
  }
}

CachedExtentRef Cache::alloc_new_non_data_extent_by_type(
  Transaction &t,        ///< [in, out] current transaction
  extent_types_t type,   ///< [in] type tag
  extent_len_t length,   ///< [in] length
  placement_hint_t hint, ///< [in] user hint
  rewrite_gen_t gen      ///< [in] rewrite generation
)
{
  LOG_PREFIX(Cache::alloc_new_non_data_extent_by_type);
  SUBDEBUGT(seastore_cache, "allocate {} 0x{:x}B, hint={}, gen={}",
            t, type, length, hint, rewrite_gen_printer_t{gen});
  ceph_assert(get_extent_category(type) == data_category_t::METADATA);
  switch (type) {
  case extent_types_t::ROOT:
    ceph_assert(0 == "ROOT is never directly alloc'd");
    return CachedExtentRef();
  case extent_types_t::LADDR_INTERNAL:
    return alloc_new_non_data_extent<lba::LBAInternalNode>(t, length, hint, gen);
  case extent_types_t::LADDR_LEAF:
    return alloc_new_non_data_extent<lba::LBALeafNode>(
      t, length, hint, gen);
  case extent_types_t::ROOT_META:
    return alloc_new_non_data_extent<RootMetaBlock>(
      t, length, hint, gen);
  case extent_types_t::ONODE_BLOCK_STAGED:
    return alloc_new_non_data_extent<onode::SeastoreNodeExtent>(
      t, length, hint, gen);
  case extent_types_t::OMAP_INNER:
    return alloc_new_non_data_extent<omap_manager::OMapInnerNode>(
      t, length, hint, gen);
  case extent_types_t::OMAP_LEAF:
    return alloc_new_non_data_extent<omap_manager::OMapLeafNode>(
      t, length, hint, gen);
  case extent_types_t::COLL_BLOCK:
    return alloc_new_non_data_extent<collection_manager::CollectionNode>(
      t, length, hint, gen);
  case extent_types_t::RETIRED_PLACEHOLDER:
    ceph_assert(0 == "impossible");
    return CachedExtentRef();
  case extent_types_t::TEST_BLOCK_PHYSICAL:
    return alloc_new_non_data_extent<TestBlockPhysical>(t, length, hint, gen);
  case extent_types_t::NONE: {
    ceph_assert(0 == "NONE is an invalid extent type");
    return CachedExtentRef();
  }
  default:
    ceph_assert(0 == "impossible");
    return CachedExtentRef();
  }
}

std::vector<CachedExtentRef> Cache::alloc_new_data_extents_by_type(
  Transaction &t,        ///< [in, out] current transaction
  extent_types_t type,   ///< [in] type tag
  extent_len_t length,   ///< [in] length
  placement_hint_t hint, ///< [in] user hint
  rewrite_gen_t gen      ///< [in] rewrite generation
)
{
  LOG_PREFIX(Cache::alloc_new_data_extents_by_type);
  SUBDEBUGT(seastore_cache, "allocate {} 0x{:x}B, hint={}, gen={}",
            t, type, length, hint, rewrite_gen_printer_t{gen});
  ceph_assert(get_extent_category(type) == data_category_t::DATA);
  std::vector<CachedExtentRef> res;
  switch (type) {
  case extent_types_t::OBJECT_DATA_BLOCK:
    {
      auto extents = alloc_new_data_extents<
	ObjectDataBlock>(t, length, hint, gen);
      res.insert(res.begin(), extents.begin(), extents.end());
    }
    return res;
  case extent_types_t::TEST_BLOCK:
    {
      auto extents = alloc_new_data_extents<
	TestBlock>(t, length, hint, gen);
      res.insert(res.begin(), extents.begin(), extents.end());
    }
    return res;
  default:
    ceph_assert(0 == "impossible");
    return res;
  }
}

CachedExtentRef Cache::duplicate_for_write(
  Transaction &t,
  CachedExtentRef i) {
  LOG_PREFIX(Cache::duplicate_for_write);
  ceph_assert(i->is_valid());
  assert(i->is_fully_loaded());

#ifndef NDEBUG
  if (i->is_logical()) {
    assert(static_cast<LogicalCachedExtent&>(*i).has_laddr());
    assert(static_cast<LogicalCachedExtent&>(*i).is_seen_by_users());
  }
#endif

  if (i->is_mutable()) {
    return i;
  }

  if (i->is_exist_clean()) {
    assert(i->is_logical());
    i->version++;
    i->state = CachedExtent::extent_state_t::EXIST_MUTATION_PENDING;
    i->last_committed_crc = i->calc_crc32c();
    // deepcopy the buffer of exist clean extent beacuse it shares
    // buffer with original clean extent.
    auto bp = i->get_bptr();
    auto nbp = ceph::bufferptr(bp.c_str(), bp.length());
    i->set_bptr(std::move(nbp));

    t.add_mutated_extent(i);
    DEBUGT("duplicate existing extent {}", t, *i);
    assert(!i->prior_instance);
    return i;
  }

  auto ret = i->duplicate_for_write(t);
  ret->pending_for_transaction = t.get_trans_id();
  ret->set_prior_instance(i);
  // duplicate_for_write won't occur after ool write finished
  assert(!i->prior_poffset);
  auto [iter, inserted] = i->mutation_pending_extents.insert(*ret);
  ceph_assert(inserted);
  if (is_root_type(ret->get_type())) {
    t.root = ret->cast<RootBlock>();
  } else {
    ret->last_committed_crc = i->last_committed_crc;
  }

  ret->version++;
  ret->state = CachedExtent::extent_state_t::MUTATION_PENDING;
  if (i->is_logical()) {
    auto& stable_extent = static_cast<LogicalCachedExtent&>(*i);
    assert(ret->is_logical());
    auto& mutate_extent = static_cast<LogicalCachedExtent&>(*ret);
    mutate_extent.set_laddr(stable_extent.get_laddr());
    assert(mutate_extent.is_seen_by_users());
  }

  t.add_mutated_extent(ret);
  DEBUGT("{} -> {}", t, *i, *ret);
  return ret;
}

record_t Cache::prepare_record(
  Transaction &t,
  const journal_seq_t &journal_head,
  const journal_seq_t &journal_dirty_tail)
{
  LOG_PREFIX(Cache::prepare_record);
  SUBTRACET(seastore_t, "enter, journal_head={}, dirty_tail={}",
            t, journal_head, journal_dirty_tail);

  auto trans_src = t.get_src();
  assert(!t.is_weak());
  assert(trans_src != Transaction::src_t::READ);
  assert(t.read_set.size() + t.num_replace_placeholder == t.read_items.size());

  auto& efforts = get_by_src(stats.committed_efforts_by_src,
                             trans_src);

  // Should be valid due to interruptible future
  io_stat_t read_stat;
  for (auto &i: t.read_set) {
    if (!i.ref->is_valid()) {
      SUBERRORT(seastore_t,
          "read_set got invalid extent, aborting -- {}", t, *i.ref);
      ceph_abort_msg("no invalid extent allowed in transactions' read_set");
    }
    get_by_ext(efforts.read_by_ext,
               i.ref->get_type()).increment(i.ref->get_length());
    read_stat.increment(i.ref->get_length());
  }
  t.clear_read_set();
  t.write_set.clear();

  record_t record(record_type_t::JOURNAL, trans_src);
  auto commit_time = seastar::lowres_system_clock::now();

  // Add new copy of mutated blocks, set_io_wait to block until written
  record.deltas.reserve(t.mutated_block_list.size());
  io_stat_t delta_stat;
  for (auto &i: t.mutated_block_list) {
    if (!i->is_valid()) {
      DEBUGT("invalid mutated extent -- {}", t, *i);
      continue;
    }
    assert(i->is_exist_mutation_pending() ||
	   i->prior_instance);
    get_by_ext(efforts.mutate_by_ext,
               i->get_type()).increment(i->get_length());

    auto delta_bl = i->get_delta();
    auto delta_length = delta_bl.length();
    i->set_modify_time(commit_time);
    DEBUGT("mutated extent with {}B delta -- {}",
	   t, delta_length, *i);
    assert(delta_length);

    if (i->is_mutation_pending()) {
      // If inplace rewrite happens from a concurrent transaction,
      // i->prior_instance will be changed from DIRTY to CLEAN implicitly, thus
      // i->prior_instance->version become 0. This won't cause conflicts
      // intentionally because inplace rewrite won't modify the shared extent.
      //
      // However, this leads to version mismatch below, thus we reset the
      // version to 1 in this case.
      if (i->prior_instance->version == 0 && i->version > 1) {
        DEBUGT("commit replace extent (inplace-rewrite) ... -- {}, prior={}",
               t, *i, *i->prior_instance);

	assert(can_inplace_rewrite(i->get_type()));
	assert(can_inplace_rewrite(i->prior_instance->get_type()));
	assert(i->prior_instance->dirty_from == JOURNAL_SEQ_MIN);
	assert(i->prior_instance->state == CachedExtent::extent_state_t::CLEAN);
	assert(i->prior_instance->get_paddr().is_absolute_random_block());
	i->version = 1;
      } else {
        DEBUGT("commit replace extent ... -- {}, prior={}",
               t, *i, *i->prior_instance);
      }
    } else {
      assert(i->is_exist_mutation_pending());
    }

    i->prepare_write();
    i->prepare_commit();

    if (i->is_mutation_pending()) {
      i->on_replace_prior();
    } // else, is_exist_mutation_pending():
      // - it doesn't have prior_instance to replace

    assert(i->get_version() > 0);
    auto final_crc = i->calc_crc32c();
    if (is_root_type(i->get_type())) {
      SUBTRACET(seastore_t, "writing out root delta {}B -- {}",
                t, delta_length, *i);
      assert(t.root == i);
      root = t.root;
      assert(root->get_paddr().is_root());
      record.push_back(
	delta_info_t{
	  extent_types_t::ROOT,
	  P_ADDR_ROOT,
	  L_ADDR_NULL,
	  0,
	  0,
	  0,
	  t.root->get_version() - 1,
	  MAX_SEG_SEQ,
	  segment_type_t::NULL_SEG,
	  std::move(delta_bl)
	});
    } else {
      auto sseq = NULL_SEG_SEQ;
      auto stype = segment_type_t::NULL_SEG;

      // FIXME: This is specific to the segmented implementation
      if (i->get_paddr().is_absolute_segmented()) {
        auto sid = i->get_paddr().as_seg_paddr().get_segment_id();
        auto sinfo = get_segment_info(sid);
        if (sinfo) {
          sseq = sinfo->seq;
          stype = sinfo->type;
        }
      }

      record.push_back(
	delta_info_t{
	  i->get_type(),
	  i->get_paddr(),
	  (i->is_logical()
	   ? i->cast<LogicalCachedExtent>()->get_laddr()
	   : L_ADDR_NULL),
	  i->last_committed_crc,
	  final_crc,
	  i->get_length(),
	  i->get_version() - 1,
	  sseq,
	  stype,
	  std::move(delta_bl)
	});
      i->last_committed_crc = final_crc;
    }

    get_by_ext(efforts.delta_bytes_by_ext,
               i->get_type()) += delta_length;
    delta_stat.increment(delta_length);
  }

  t.for_each_finalized_fresh_block([](auto &e) {
    // fresh blocks' `prepare_commit` must be invoked before
    // retiering extents, this is because logical linked tree
    // nodes needs to access their prior instances in this
    // phase if they are rewritten.
    e->prepare_commit();
  });

  /*
   * We can only update extent states after the logical linked tree is
   * resolved:
   * - on_replace_prior()
   * - prepare_commit()
   */
  for (auto &i: t.mutated_block_list) {
    if (i->is_valid()) {
      if (i->is_mutation_pending()) {
        i->set_io_wait(CachedExtent::extent_state_t::DIRTY);
        commit_replace_extent(t, i, i->prior_instance);
      } // else, is_exist_mutation_pending():
        // - it doesn't have prior_instance to replace
        // - and add_extent() atomically below
        // - set_io_wait(DIRTY) atomically below
    }
  }

  // Transaction is now a go, set up in-memory cache state
  // invalidate now invalid blocks
  io_stat_t retire_stat;
  std::vector<alloc_delta_t> alloc_deltas;
  alloc_delta_t rel_delta;
  backref_entry_refs_t backref_entries;
  rel_delta.op = alloc_delta_t::op_types_t::CLEAR;
  for (auto &i: t.retired_set) {
    auto &extent = i.extent;
    get_by_ext(efforts.retire_by_ext,
               extent->get_type()).increment(extent->get_length());
    retire_stat.increment(extent->get_length());
    DEBUGT("retired and remove extent {}~0x{:x} -- {}",
	   t, extent->get_paddr(), extent->get_length(), *extent);
    commit_retire_extent(t, extent);

    // Note: commit extents and backref allocations in the same place
    if (is_backref_mapped_type(extent->get_type()) ||
	is_retired_placeholder_type(extent->get_type())) {
      DEBUGT("backref_entry free {}~0x{:x}",
	     t,
	     extent->get_paddr(),
	     extent->get_length());
      rel_delta.alloc_blk_ranges.emplace_back(
	alloc_blk_t::create_retire(
	  extent->get_paddr(),
	  extent->get_length(),
	  extent->get_type()));
      backref_entries.emplace_back(
	backref_entry_t::create_retire(
	  extent->get_paddr(),
	  extent->get_length(),
	  extent->get_type()));
    } else if (is_backref_node(extent->get_type())) {
      remove_backref_extent(extent->get_paddr());
    } else {
      ERRORT("Got unexpected extent type: {}", t, *extent);
      ceph_abort_msg("imposible");
    }
  }
  alloc_deltas.emplace_back(std::move(rel_delta));

  record.extents.reserve(t.inline_block_list.size());
  io_stat_t fresh_stat;
  io_stat_t fresh_invalid_stat;
  alloc_delta_t alloc_delta;
  alloc_delta.op = alloc_delta_t::op_types_t::SET;
  for (auto &i: t.inline_block_list) {
    if (!i->is_valid()) {
      DEBUGT("invalid fresh inline extent -- {}", t, *i);
      fresh_invalid_stat.increment(i->get_length());
      get_by_ext(efforts.fresh_invalid_by_ext,
                 i->get_type()).increment(i->get_length());
    } else {
      TRACET("fresh inline extent -- {}", t, *i);
    }
    fresh_stat.increment(i->get_length());
    get_by_ext(efforts.fresh_inline_by_ext,
               i->get_type()).increment(i->get_length());
#ifdef UNIT_TESTS_BUILT
    assert(i->is_inline() || i->get_paddr().is_fake());
#else
    assert(i->is_inline());
#endif

    bufferlist bl;
    i->prepare_write();
    bl.append(i->get_bptr());
    if (is_root_type(i->get_type())) {
      ceph_assert(0 == "ROOT never gets written as a fresh block");
    }

    assert(bl.length() == i->get_length());
    auto modify_time = i->get_modify_time();
    if (modify_time == NULL_TIME) {
      modify_time = commit_time;
    }
    laddr_t fresh_laddr;
    if (i->is_logical()) {
      fresh_laddr = i->cast<LogicalCachedExtent>()->get_laddr();
    } else if (is_lba_node(i->get_type())) {
      fresh_laddr = i->cast<lba::LBANode>()->get_node_meta().begin;
    } else {
      fresh_laddr = L_ADDR_NULL;
    }
    record.push_back(extent_t{
	i->get_type(),
	fresh_laddr,
	std::move(bl)
      },
      modify_time);

    if (!i->is_valid()) {
      continue;
    }
    if (is_backref_mapped_type(i->get_type())) {
      laddr_t alloc_laddr;
      if (i->is_logical()) {
	alloc_laddr = i->cast<LogicalCachedExtent>()->get_laddr();
      } else if (is_lba_node(i->get_type())) {
	alloc_laddr = i->cast<lba::LBANode>()->get_node_meta().begin;
      } else {
	assert(i->get_type() == extent_types_t::TEST_BLOCK_PHYSICAL);
	alloc_laddr = L_ADDR_MIN;
      }
      alloc_delta.alloc_blk_ranges.emplace_back(
	alloc_blk_t::create_alloc(
	  i->get_paddr(),
	  alloc_laddr,
	  i->get_length(),
	  i->get_type()));
    }
    i->set_io_wait(CachedExtent::extent_state_t::CLEAN);
    // Note, paddr is known until complete_commit(),
    // so add_extent() later.
  }

  for (auto &i: t.ool_block_list) {
    TRACET("fresh ool extent -- {}", t, *i);
    ceph_assert(i->is_valid());
    assert(i->get_paddr().is_absolute());
    get_by_ext(efforts.fresh_ool_by_ext,
               i->get_type()).increment(i->get_length());
    if (is_backref_mapped_type(i->get_type())) {
      laddr_t alloc_laddr;
      if (i->is_logical()) {
        alloc_laddr = i->cast<LogicalCachedExtent>()->get_laddr();
      } else {
        assert(is_lba_node(i->get_type()));
        alloc_laddr = i->cast<lba::LBANode>()->get_node_meta().begin;
      }
      alloc_delta.alloc_blk_ranges.emplace_back(
	alloc_blk_t::create_alloc(
	  i->get_paddr(),
	  alloc_laddr,
	  i->get_length(),
	  i->get_type()));
    }
    i->set_io_wait(CachedExtent::extent_state_t::CLEAN);
    // Note, paddr is (can be) known until complete_commit(),
    // so add_extent() later.
  }

  for (auto &i: t.inplace_ool_block_list) {
    if (!i->is_valid()) {
      continue;
    }
    assert(i->state == CachedExtent::extent_state_t::DIRTY);
    assert(i->version > 0);
    assert(i->pending_for_transaction == TRANS_ID_NULL);
    assert(!i->prior_instance);
    remove_from_dirty(i, &trans_src);
    // set the version to zero because the extent state is now clean
    // in order to handle this transparently
    i->version = 0;
    i->dirty_from = JOURNAL_SEQ_MIN;
    // no set_io_wait(), skip complete_commit()
    assert(!i->is_pending_io());
    i->state = CachedExtent::extent_state_t::CLEAN;
    assert(i->is_logical());
    i->clear_modified_region();
    touch_extent_fully(*i, &trans_src, t.get_cache_hint());
    DEBUGT("inplace rewrite ool block is commmitted -- {}", t, *i);
  }

  auto existing_stats = t.get_existing_block_stats();
  DEBUGT("total existing blocks num: {}, exist clean num: {}, "
	 "exist mutation pending num: {}",
	 t,
	 existing_stats.valid_num,
	 existing_stats.clean_num,
	 existing_stats.mutated_num);
  for (auto &i: t.existing_block_list) {
    assert(is_logical_type(i->get_type()));
    if (!i->is_valid()) {
      continue;
    }

    if (i->is_exist_clean()) {
      assert(i->version == 0);
      assert(!i->prior_instance);
      // no set_io_wait(), skip complete_commit()
      assert(!i->is_pending_io());
      i->pending_for_transaction = TRANS_ID_NULL;
      i->state = CachedExtent::extent_state_t::CLEAN;
    } else {
      assert(i->is_exist_mutation_pending());
      i->set_io_wait(CachedExtent::extent_state_t::DIRTY);
    }

    // exist mutation pending extents must be in t.mutated_block_list
    add_extent(i);
    const auto t_src = t.get_src();
    if (i->is_stable_dirty()) {
      add_to_dirty(i, &t_src);
    } else {
      touch_extent_fully(*i, &t_src, t.get_cache_hint());
    }

    alloc_delta.alloc_blk_ranges.emplace_back(
      alloc_blk_t::create_alloc(
	i->get_paddr(),
	i->cast<LogicalCachedExtent>()->get_laddr(),
	i->get_length(),
	i->get_type()));

    // Note: commit extents and backref allocations in the same place
    // Note: remapping is split into 2 steps, retire and alloc, they must be
    //       committed atomically together
    backref_entries.emplace_back(
      backref_entry_t::create_alloc(
	i->get_paddr(),
	i->cast<LogicalCachedExtent>()->get_laddr(),
	i->get_length(),
	i->get_type()));
  }

  alloc_deltas.emplace_back(std::move(alloc_delta));

  for (auto b : alloc_deltas) {
    bufferlist bl;
    encode(b, bl);
    delta_info_t delta;
    delta.type = extent_types_t::ALLOC_INFO;
    delta.bl = bl;
    record.push_back(std::move(delta));
  }

  if (is_background_transaction(trans_src)) {
    assert(journal_head != JOURNAL_SEQ_NULL);
    assert(journal_dirty_tail != JOURNAL_SEQ_NULL);
    journal_seq_t dirty_tail;
    auto maybe_dirty_tail = get_oldest_dirty_from();
    if (!maybe_dirty_tail.has_value()) {
      dirty_tail = journal_head;
      SUBINFOT(seastore_t, "dirty_tail all trimmed, set to head {}, src={}",
               t, dirty_tail, trans_src);
    } else if (*maybe_dirty_tail == JOURNAL_SEQ_NULL) {
      dirty_tail = journal_dirty_tail;
      SUBINFOT(seastore_t, "dirty_tail is pending, set to {}, src={}",
               t, dirty_tail, trans_src);
    } else {
      dirty_tail = *maybe_dirty_tail;
    }
    ceph_assert(dirty_tail != JOURNAL_SEQ_NULL);
    journal_seq_t alloc_tail;
    auto maybe_alloc_tail = get_oldest_backref_dirty_from();
    if (!maybe_alloc_tail.has_value()) {
      // FIXME: the replay point of the allocations requires to be accurate.
      // Setting the alloc_tail to get_journal_head() cannot skip replaying the
      // last unnecessary record.
      alloc_tail = journal_head;
      SUBINFOT(seastore_t, "alloc_tail all trimmed, set to head {}, src={}",
               t, alloc_tail, trans_src);
    } else if (*maybe_alloc_tail == JOURNAL_SEQ_NULL) {
      ceph_abort_msg("impossible");
    } else {
      alloc_tail = *maybe_alloc_tail;
    }
    ceph_assert(alloc_tail != JOURNAL_SEQ_NULL);
    auto tails = journal_tail_delta_t{alloc_tail, dirty_tail};
    SUBDEBUGT(seastore_t, "update tails as delta {}", t, tails);
    bufferlist bl;
    encode(tails, bl);
    delta_info_t delta;
    delta.type = extent_types_t::JOURNAL_TAIL;
    delta.bl = bl;
    record.push_back(std::move(delta));
  }

  apply_backref_mset(backref_entries);
  t.set_backref_entries(std::move(backref_entries));

  ceph_assert(t.get_fresh_block_stats().num ==
              t.inline_block_list.size() +
              t.ool_block_list.size() +
              t.num_delayed_invalid_extents +
	      t.num_allocated_invalid_extents);

  auto& ool_stats = t.get_ool_write_stats();
  ceph_assert(ool_stats.extents.num == t.ool_block_list.size() +
    t.inplace_ool_block_list.size());

  if (record.is_empty()) {
    SUBINFOT(seastore_t,
        "record to submit is empty, src={}", t, trans_src);
    assert(t.onode_tree_stats.is_clear());
    assert(t.omap_tree_stats.is_clear());
    assert(t.lba_tree_stats.is_clear());
    assert(t.backref_tree_stats.is_clear());
    assert(ool_stats.is_clear());
  }

  if (record.modify_time == NULL_TIME) {
    record.modify_time = commit_time;
  }

  SUBDEBUGT(seastore_t,
      "commit H{} dirty_from={}, alloc_from={}, "
      "{} read, {} fresh with {} invalid, "
      "{} delta, {} retire, {}(md={}B, data={}B) ool-records, "
      "{}B md, {}B data, modify_time={}",
      t, (void*)&t.get_handle(),
      get_oldest_dirty_from().value_or(JOURNAL_SEQ_NULL),
      get_oldest_backref_dirty_from().value_or(JOURNAL_SEQ_NULL),
      read_stat,
      fresh_stat,
      fresh_invalid_stat,
      delta_stat,
      retire_stat,
      ool_stats.num_records,
      ool_stats.md_bytes,
      ool_stats.get_data_bytes(),
      record.size.get_raw_mdlength(),
      record.size.dlength,
      sea_time_point_printer_t{record.modify_time});
  if (is_background_transaction(trans_src)) {
    // background transaction won't contain any onode tree operations
    assert(t.onode_tree_stats.is_clear());
    assert(t.omap_tree_stats.is_clear());
  } else {
    if (t.onode_tree_stats.depth) {
      stats.onode_tree_depth = t.onode_tree_stats.depth;
    }
    if (t.omap_tree_stats.depth) {
      stats.omap_tree_depth = t.omap_tree_stats.depth;
    }
    stats.onode_tree_extents_num += t.onode_tree_stats.extents_num_delta;
    ceph_assert(stats.onode_tree_extents_num >= 0);
    get_by_src(stats.committed_onode_tree_efforts, trans_src
        ).increment(t.onode_tree_stats);
    stats.omap_tree_extents_num += t.omap_tree_stats.extents_num_delta;
    ceph_assert(stats.omap_tree_extents_num >= 0);
    get_by_src(stats.committed_omap_tree_efforts, trans_src
        ).increment(t.omap_tree_stats);
  }

  if (t.lba_tree_stats.depth) {
    stats.lba_tree_depth = t.lba_tree_stats.depth;
  }
  stats.lba_tree_extents_num += t.lba_tree_stats.extents_num_delta;
  ceph_assert(stats.lba_tree_extents_num >= 0);
  get_by_src(stats.committed_lba_tree_efforts, trans_src
      ).increment(t.lba_tree_stats);
  if (t.backref_tree_stats.depth) {
    stats.backref_tree_depth = t.backref_tree_stats.depth;
  }
  stats.backref_tree_extents_num += t.backref_tree_stats.extents_num_delta;
  ceph_assert(stats.backref_tree_extents_num >= 0);
  get_by_src(stats.committed_backref_tree_efforts, trans_src
      ).increment(t.backref_tree_stats);

  ++(efforts.num_trans);
  efforts.num_ool_records += ool_stats.num_records;
  efforts.ool_record_metadata_bytes += ool_stats.md_bytes;
  efforts.ool_record_data_bytes += ool_stats.get_data_bytes();
  efforts.inline_record_metadata_bytes +=
    (record.size.get_raw_mdlength() - record.get_delta_size());

  auto &rewrite_stats = t.get_rewrite_stats();
  if (trans_src == Transaction::src_t::TRIM_DIRTY) {
    stats.trim_rewrites.add(rewrite_stats);
  } else if (trans_src == Transaction::src_t::CLEANER_MAIN ||
             trans_src == Transaction::src_t::CLEANER_COLD) {
    stats.reclaim_rewrites.add(rewrite_stats);
  } else {
    assert(rewrite_stats.is_clear());
  }

  return record;
}

void Cache::apply_backref_byseq(
  backref_entry_refs_t&& backref_entries,
  const journal_seq_t& seq)
{
  LOG_PREFIX(Cache::apply_backref_byseq);
  DEBUG("backref_entry apply {} entries at {}",
	backref_entries.size(), seq);
  assert(seq != JOURNAL_SEQ_NULL);
  if (backref_entries.empty()) {
    return;
  }
  if (backref_entryrefs_by_seq.empty()) {
    backref_entryrefs_by_seq.insert(
      backref_entryrefs_by_seq.end(),
      {seq, std::move(backref_entries)});
    return;
  }
  auto last = backref_entryrefs_by_seq.rbegin();
  assert(last->first <= seq);
  if (last->first == seq) {
    last->second.insert(
      last->second.end(),
      std::make_move_iterator(backref_entries.begin()),
      std::make_move_iterator(backref_entries.end()));
  } else {
    assert(last->first < seq);
    backref_entryrefs_by_seq.insert(
      backref_entryrefs_by_seq.end(),
      {seq, std::move(backref_entries)});
  }
}

void Cache::complete_commit(
  Transaction &t,
  paddr_t final_block_start,
  journal_seq_t start_seq)
{
  LOG_PREFIX(Cache::complete_commit);
  SUBTRACET(seastore_t, "final_block_start={}, start_seq={}",
            t, final_block_start, start_seq);

  backref_entry_refs_t backref_entries;
  t.for_each_finalized_fresh_block([&](const CachedExtentRef &i) {
    if (!i->is_valid()) {
      return;
    }

    assert(i->is_stable_clean_pending());
    bool is_inline = false;
    if (i->is_inline()) {
      is_inline = true;
      i->set_paddr(final_block_start.add_relative(i->get_paddr()));
    }
#ifndef NDEBUG
    if (i->get_paddr().is_root() || epm.get_checksum_needed(i->get_paddr())) {
      assert(i->get_last_committed_crc() == i->calc_crc32c());
    } else {
      assert(i->get_last_committed_crc() == CRC_NULL);
    }
#endif
    i->pending_for_transaction = TRANS_ID_NULL;
    i->on_initial_write();
    i->reset_prior_instance();
    DEBUGT("add extent as fresh, inline={} -- {}",
	   t, is_inline, *i);
    i->invalidate_hints();
    add_extent(i);
    const auto t_src = t.get_src();
    touch_extent_fully(*i, &t_src, t.get_cache_hint());
    i->complete_io();
    epm.commit_space_used(i->get_paddr(), i->get_length());

    // Note: commit extents and backref allocations in the same place
    if (is_backref_mapped_type(i->get_type())) {
      DEBUGT("backref_entry alloc {}~0x{:x}",
	     t,
	     i->get_paddr(),
	     i->get_length());
      laddr_t alloc_laddr;
      if (i->is_logical()) {
	alloc_laddr = i->cast<LogicalCachedExtent>()->get_laddr();
      } else if (is_lba_node(i->get_type())) {
	alloc_laddr = i->cast<lba::LBANode>()->get_node_meta().begin;
      } else {
	assert(i->get_type() == extent_types_t::TEST_BLOCK_PHYSICAL);
	alloc_laddr = L_ADDR_MIN;
      }
      backref_entries.emplace_back(
	backref_entry_t::create_alloc(
	  i->get_paddr(),
	  alloc_laddr,
	  i->get_length(),
	  i->get_type()));
    } else if (is_backref_node(i->get_type())) {
	add_backref_extent(
	  i->get_paddr(),
	  i->cast<backref::BackrefNode>()->get_node_meta().begin,
	  i->get_type());
    } else {
      ERRORT("{}", t, *i);
      ceph_abort_msg("not possible");
    }
  });

  // Add new copy of mutated blocks, set_io_wait to block until written
  for (auto &i: t.mutated_block_list) {
    if (!i->is_valid()) {
      continue;
    }
    assert(i->is_stable_dirty());
    assert(i->is_pending_io());
    assert(i->io_wait->from_state == CachedExtent::extent_state_t::EXIST_MUTATION_PENDING
           || (i->io_wait->from_state == CachedExtent::extent_state_t::MUTATION_PENDING
               && i->prior_instance));
    i->on_delta_write(final_block_start);
    i->pending_for_transaction = TRANS_ID_NULL;
    i->reset_prior_instance();
    assert(i->version > 0);
    if (i->version == 1 || is_root_type(i->get_type())) {
      i->dirty_from = start_seq;
      DEBUGT("commit extent done, become dirty -- {}", t, *i);
    } else {
      DEBUGT("commit extent done -- {}", t, *i);
    }
    i->complete_io();
  }

  for (auto &i: t.retired_set) {
    auto &extent = i.extent;
    epm.mark_space_free(extent->get_paddr(), extent->get_length());
  }
  for (auto &i: t.existing_block_list) {
    if (!i->is_valid()) {
      continue;
    }
    epm.mark_space_used(i->get_paddr(), i->get_length());
  }
  for (auto &i: t.pre_alloc_list) {
    if (!i->is_valid()) {
      epm.mark_space_free(i->get_paddr(), i->get_length());
    }
  }

  last_commit = start_seq;

  apply_backref_byseq(t.move_backref_entries(), start_seq);
  commit_backref_entries(std::move(backref_entries), start_seq);
}

void Cache::init()
{
  LOG_PREFIX(Cache::init);
  if (root) {
    // initial creation will do mkfs followed by mount each of which calls init
    DEBUG("remove extent -- prv_root={}", *root);
    remove_extent(root, nullptr);
    root = nullptr;
  }
  root = CachedExtent::make_cached_extent_ref<RootBlock>();
  // Make it simpler to keep root dirty
  root->init(CachedExtent::extent_state_t::DIRTY,
             P_ADDR_ROOT,
             PLACEMENT_HINT_NULL,
             NULL_GENERATION,
             TRANS_ID_NULL);
  root->set_modify_time(seastar::lowres_system_clock::now());
  INFO("init root -- {}", *root);
  add_extent(root);
  add_to_dirty(root, nullptr);
}

Cache::mkfs_iertr::future<> Cache::mkfs(Transaction &t)
{
  LOG_PREFIX(Cache::mkfs);
  INFOT("create root", t);
  return get_root(t).si_then([this, &t](auto croot) {
    duplicate_for_write(t, croot);
    return mkfs_iertr::now();
  }).handle_error_interruptible(
    mkfs_iertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error in Cache::mkfs"
    }
  );
}

Cache::close_ertr::future<> Cache::close()
{
  LOG_PREFIX(Cache::close);
  INFO("close with {}({}B) dirty, dirty_from={}, alloc_from={}, "
       "{}({}B) pinned extents, totally {}({}B) indexed extents",
       dirty.size(),
       stats.dirty_bytes,
       get_oldest_dirty_from().value_or(JOURNAL_SEQ_NULL),
       get_oldest_backref_dirty_from().value_or(JOURNAL_SEQ_NULL),
       pinboard->get_current_num_extents(),
       pinboard->get_current_size_bytes(),
       extents_index.size(),
       extents_index.get_bytes());
  root.reset();
  clear_dirty();
  backref_extents.clear();
  backref_entryrefs_by_seq.clear();
  pinboard->clear();
  return close_ertr::now();
}

Cache::replay_delta_ret
Cache::replay_delta(
  journal_seq_t journal_seq,
  paddr_t record_base,
  const delta_info_t &delta,
  const journal_seq_t &dirty_tail,
  const journal_seq_t &alloc_tail,
  sea_time_point modify_time)
{
  LOG_PREFIX(Cache::replay_delta);
  assert(dirty_tail != JOURNAL_SEQ_NULL);
  assert(alloc_tail != JOURNAL_SEQ_NULL);
  ceph_assert(modify_time != NULL_TIME);

  // FIXME: This is specific to the segmented implementation
  /* The journal may validly contain deltas for extents in
   * since released segments.  We can detect those cases by
   * checking whether the segment in question currently has a
   * sequence number > the current journal segment seq. We can
   * safetly skip these deltas because the extent must already
   * have been rewritten.
   */
  if (delta.paddr.is_absolute_segmented()) {
    auto& seg_addr = delta.paddr.as_seg_paddr();
    auto seg_info = get_segment_info(seg_addr.get_segment_id());
    if (seg_info) {
      auto delta_paddr_segment_seq = seg_info->seq;
      auto delta_paddr_segment_type = seg_info->type;
      if (delta_paddr_segment_seq != delta.ext_seq ||
          delta_paddr_segment_type != delta.seg_type) {
        DEBUG("delta is obsolete, delta_paddr_segment_seq={},"
              " delta_paddr_segment_type={} -- {}",
              segment_seq_printer_t{delta_paddr_segment_seq},
              delta_paddr_segment_type,
              delta);
        return replay_delta_ertr::make_ready_future<std::pair<bool, CachedExtentRef>>(
	  std::make_pair(false, nullptr));
      }
    }
  }

  if (delta.type == extent_types_t::JOURNAL_TAIL) {
    // this delta should have been dealt with during segment cleaner mounting
    return replay_delta_ertr::make_ready_future<std::pair<bool, CachedExtentRef>>(
      std::make_pair(false, nullptr));
  }

  // replay alloc
  if (delta.type == extent_types_t::ALLOC_INFO) {
    if (journal_seq < alloc_tail) {
      DEBUG("journal_seq {} < alloc_tail {}, don't replay {}",
	journal_seq, alloc_tail, delta);
      return replay_delta_ertr::make_ready_future<std::pair<bool, CachedExtentRef>>(
	std::make_pair(false, nullptr));
    }

    alloc_delta_t alloc_delta;
    decode(alloc_delta, delta.bl);
    backref_entry_refs_t backref_entries;
    for (auto &alloc_blk : alloc_delta.alloc_blk_ranges) {
      if (alloc_blk.paddr.is_record_relative()) {
	alloc_blk.paddr = record_base.add_relative(alloc_blk.paddr);
      } else {
        ceph_assert(alloc_blk.paddr.is_absolute());
      }
      DEBUG("replay alloc_blk {}~0x{:x} {}, journal_seq: {}",
	alloc_blk.paddr, alloc_blk.len, alloc_blk.laddr, journal_seq);
      backref_entries.emplace_back(
	backref_entry_t::create(alloc_blk));
    }
    commit_backref_entries(std::move(backref_entries), journal_seq);
    return replay_delta_ertr::make_ready_future<std::pair<bool, CachedExtentRef>>(
      std::make_pair(true, nullptr));
  }

  // replay dirty
  if (journal_seq < dirty_tail) {
    DEBUG("journal_seq {} < dirty_tail {}, don't replay {}",
      journal_seq, dirty_tail, delta);
    return replay_delta_ertr::make_ready_future<std::pair<bool, CachedExtentRef>>(
      std::make_pair(false, nullptr));
  }

  if (is_root_type(delta.type)) {
    TRACE("replay root delta at {} {}, remove extent ... -- {}, prv_root={}",
          journal_seq, record_base, delta, *root);
    ceph_assert(delta.paddr.is_root());
    remove_extent(root, nullptr);
    root->apply_delta_and_adjust_crc(record_base, delta.bl);
    root->dirty_from = journal_seq;
    root->state = CachedExtent::extent_state_t::DIRTY;
    root->version = 1; // shouldn't be 0 as a dirty extent
    DEBUG("replayed root delta at {} {}, add extent -- {}, root={}",
          journal_seq, record_base, delta, *root);
    root->set_modify_time(modify_time);
    add_extent(root);
    add_to_dirty(root, nullptr);
    return replay_delta_ertr::make_ready_future<std::pair<bool, CachedExtentRef>>(
      std::make_pair(true, root));
  } else {
    ceph_assert(delta.paddr.is_absolute());
    auto _get_extent_if_cached = [this](paddr_t addr)
      -> get_extent_ertr::future<CachedExtentRef> {
      // replay is not included by the cache hit metrics
      auto ret = query_cache(addr);
      if (ret) {
        // no retired-placeholder should be exist yet because no transaction
        // has been created.
        assert(!is_retired_placeholder_type(ret->get_type()));
        return ret->wait_io().then([ret] {
          return ret;
        });
      } else {
        return seastar::make_ready_future<CachedExtentRef>();
      }
    };
    auto extent_fut = (delta.pversion == 0 ?
      do_get_caching_extent_by_type(
        delta.type,
        delta.paddr,
        delta.laddr,
        delta.length,
        [](CachedExtent &) {},
        [this, laddr=delta.laddr](CachedExtent &ext) {
          assert(ext.is_logical() == (laddr != L_ADDR_NULL));
          if (ext.is_logical()) {
            // ExtentPinboardTwoQ requires the laddr is set for warm out queue.
            ext.cast<LogicalCachedExtent>()->set_laddr(laddr);
          }
          // replay is not included by the cache hit metrics
          touch_extent_fully(ext, nullptr, CACHE_HINT_TOUCH);
        },
        nullptr) :
      _get_extent_if_cached(
	delta.paddr)
    ).handle_error(
      replay_delta_ertr::pass_further{},
      crimson::ct_error::assert_all{
	"Invalid error in Cache::replay_delta"
      }
    );
    return extent_fut.safe_then([=, this, &delta](auto extent) {
      if (!extent) {
	DEBUG("replay extent is not present, so delta is obsolete at {} {} -- {}",
	      journal_seq, record_base, delta);
	assert(delta.pversion > 0);
	return replay_delta_ertr::make_ready_future<std::pair<bool, CachedExtentRef>>(
	  std::make_pair(false, nullptr));
      }

      DEBUG("replay extent delta at {} {} ... -- {}, prv_extent={}",
            journal_seq, record_base, delta, *extent);

      if (delta.paddr.is_absolute_segmented() ||
	  !can_inplace_rewrite(delta.type)) {
	ceph_assert_always(extent->last_committed_crc == delta.prev_crc);
	assert(extent->version == delta.pversion);
	extent->apply_delta_and_adjust_crc(record_base, delta.bl);
	extent->set_modify_time(modify_time);
	ceph_assert_always(extent->last_committed_crc == delta.final_crc);
      } else {
	assert(delta.paddr.is_absolute_random_block());
	// see prepare_record(), inplace rewrite might cause version mismatch
	extent->apply_delta_and_adjust_crc(record_base, delta.bl);
	extent->set_modify_time(modify_time);
	// crc will be checked after journal replay is done
      }

      extent->version++;
      if (extent->version == 1) {
	extent->dirty_from = journal_seq;
        DEBUG("replayed extent delta at {} {}, become dirty -- {}, extent={}" ,
              journal_seq, record_base, delta, *extent);
      } else {
        DEBUG("replayed extent delta at {} {} -- {}, extent={}" ,
              journal_seq, record_base, delta, *extent);
      }
      mark_dirty(extent);
      return replay_delta_ertr::make_ready_future<std::pair<bool, CachedExtentRef>>(
	std::make_pair(true, extent));
    });
  }
}

Cache::get_next_dirty_extents_ret Cache::get_next_dirty_extents(
  Transaction &t,
  journal_seq_t seq,
  size_t max_bytes)
{
  LOG_PREFIX(Cache::get_next_dirty_extents);
  if (dirty.empty()) {
    DEBUGT("max_bytes={}B, seq={}, dirty is empty",
           t, max_bytes, seq);
  } else {
    DEBUGT("max_bytes={}B, seq={}, dirty_from={}",
           t, max_bytes, seq, dirty.begin()->get_dirty_from());
  }
  std::vector<CachedExtentRef> cand;
  size_t bytes_so_far = 0;
  for (auto i = dirty.begin();
       i != dirty.end() && bytes_so_far < max_bytes;
       ++i) {
    auto dirty_from = i->get_dirty_from();
    //dirty extents must be fully loaded
    assert(i->is_fully_loaded());
    if (unlikely(dirty_from == JOURNAL_SEQ_NULL)) {
      ERRORT("got dirty extent with JOURNAL_SEQ_NULL -- {}", t, *i);
      ceph_abort();
    }
    if (dirty_from < seq) {
      TRACET("next extent -- {}", t, *i);
      if (!cand.empty() && cand.back()->get_dirty_from() > dirty_from) {
	ERRORT("dirty extents are not ordered by dirty_from -- last={}, next={}",
               t, *cand.back(), *i);
        ceph_abort();
      }
      bytes_so_far += i->get_length();
      cand.push_back(&*i);
    } else {
      break;
    }
  }
  return seastar::do_with(
    std::move(cand),
    decltype(cand)(),
    [FNAME, this, &t](auto &cand, auto &ret) {
      return trans_intr::do_for_each(
	cand,
	[FNAME, this, &t, &ret](auto &ext) {
	  TRACET("waiting on extent -- {}", t, *ext);
	  return trans_intr::make_interruptible(
	    ext->wait_io()
	  ).then_interruptible([FNAME, this, ext, &t, &ret] {
	    if (!ext->is_valid()) {
	      ++(get_by_src(stats.trans_conflicts_by_unknown, t.get_src()));
	      mark_transaction_conflicted(t, *ext);
	      return;
	    }

	    CachedExtentRef on_transaction;
	    auto result = t.get_extent(ext->get_paddr(), &on_transaction);
	    if (result == Transaction::get_extent_ret::ABSENT) {
	      DEBUGT("extent is absent on t -- {}", t, *ext);
	      t.add_to_read_set(ext);
	      if (is_root_type(ext->get_type())) {
		if (t.root) {
		  assert(&*t.root == &*ext);
		  ceph_assert(0 == "t.root would have to already be in the read set");
		} else {
		  assert(&*ext == &*root);
		  t.root = root;
		}
	      }
	      ret.push_back(ext);
	    } else if (result == Transaction::get_extent_ret::PRESENT) {
	      DEBUGT("extent is present on t -- {}, on t {}", t, *ext, *on_transaction);
	      ret.push_back(on_transaction);
	    } else {
	      assert(result == Transaction::get_extent_ret::RETIRED);
	      DEBUGT("extent is retired on t -- {}", t, *ext);
	    }
	  });
	}).then_interruptible([&ret] {
	  return std::move(ret);
	});
    });
}

Cache::get_root_ret Cache::get_root(Transaction &t)
{
  LOG_PREFIX(Cache::get_root);
  if (t.root) {
    TRACET("root already on t -- {}", t, *t.root);
    return t.root->wait_io().then([&t] {
      return get_root_iertr::make_ready_future<RootBlockRef>(
	t.root);
    });
  } else {
    DEBUGT("root not on t -- {}", t, *root);
    t.root = root;
    t.add_to_read_set(root);
    return root->wait_io().then([root=root] {
      return get_root_iertr::make_ready_future<RootBlockRef>(
	root);
    });
  }
}

Cache::get_extent_ertr::future<CachedExtentRef>
Cache::do_get_caching_extent_by_type(
  extent_types_t type,
  paddr_t offset,
  laddr_t laddr,
  extent_len_t length,
  extent_init_func_t &&extent_init_func,
  extent_init_func_t &&on_cache,
  const Transaction::src_t* p_src)
{
  switch (type) {
  case extent_types_t::ROOT:
    ceph_assert(0 == "ROOT is never directly read");
    return get_extent_ertr::make_ready_future<CachedExtentRef>();
  case extent_types_t::BACKREF_INTERNAL:
    return do_get_caching_extent<backref::BackrefInternalNode>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::BACKREF_LEAF:
    return do_get_caching_extent<backref::BackrefLeafNode>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::LADDR_INTERNAL:
    return do_get_caching_extent<lba::LBAInternalNode>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::LADDR_LEAF:
    return do_get_caching_extent<lba::LBALeafNode>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::ROOT_META:
    return do_get_caching_extent<RootMetaBlock>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::OMAP_INNER:
    return do_get_caching_extent<omap_manager::OMapInnerNode>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::OMAP_LEAF:
    return do_get_caching_extent<omap_manager::OMapLeafNode>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::COLL_BLOCK:
    return do_get_caching_extent<collection_manager::CollectionNode>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::ONODE_BLOCK_STAGED:
    return do_get_caching_extent<onode::SeastoreNodeExtent>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::OBJECT_DATA_BLOCK:
    return do_get_caching_extent<ObjectDataBlock>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::RETIRED_PLACEHOLDER:
    ceph_assert(0 == "impossible");
    return get_extent_ertr::make_ready_future<CachedExtentRef>();
  case extent_types_t::TEST_BLOCK:
    return do_get_caching_extent<TestBlock>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::TEST_BLOCK_PHYSICAL:
    return do_get_caching_extent<TestBlockPhysical>(
      offset, length, std::move(extent_init_func), std::move(on_cache), p_src
    ).safe_then([](auto extent) {
      return CachedExtentRef(extent.detach(), false /* add_ref */);
    });
  case extent_types_t::NONE: {
    ceph_assert(0 == "NONE is an invalid extent type");
    return get_extent_ertr::make_ready_future<CachedExtentRef>();
  }
  default:
    ceph_assert(0 == "impossible");
    return get_extent_ertr::make_ready_future<CachedExtentRef>();
  }
}

cache_stats_t Cache::get_stats(
  bool report_detail, double seconds) const
{
  LOG_PREFIX(Cache::get_stats);

  cache_stats_t ret;
  pinboard->get_stats(ret, report_detail, seconds);

  /*
   * dirty stats
   * rewrite stats
   * index stats
   * access stats
   */

  ret.dirty_sizes = cache_size_stats_t{stats.dirty_bytes, dirty.size()};
  ret.dirty_io = stats.dirty_io;
  ret.dirty_io.minus(last_dirty_io);
  ret.access = stats.access;
  ret.access.minus(last_access);

  if (report_detail && seconds != 0) {
    counter_by_src_t<counter_by_extent_t<dirty_io_stats_t> >
      _trans_io_by_src_ext = stats.dirty_io_by_src_ext;
    counter_by_src_t<dirty_io_stats_t> trans_io_by_src;
    for (uint8_t _src=0; _src<TRANSACTION_TYPE_MAX; ++_src) {
      auto src = static_cast<transaction_type_t>(_src);
      auto& io_by_ext = get_by_src(_trans_io_by_src_ext, src);
      const auto& last_io_by_ext = get_by_src(last_dirty_io_by_src_ext, src);
      auto& trans_io_per_src = get_by_src(trans_io_by_src, src);
      for (uint8_t _ext=0; _ext<EXTENT_TYPES_MAX; ++_ext) {
        auto ext = static_cast<extent_types_t>(_ext);
        auto& extent_io = get_by_ext(io_by_ext, ext);
        const auto& last_extent_io = get_by_ext(last_io_by_ext, ext);
        extent_io.minus(last_extent_io);
        trans_io_per_src.add(extent_io);
      }
    }

    std::ostringstream oss;
    oss << "\ndirty total" << ret.dirty_sizes;
    cache_size_stats_t data_sizes;
    cache_size_stats_t mdat_sizes;
    cache_size_stats_t phys_sizes;
    for (uint8_t _ext=0; _ext<EXTENT_TYPES_MAX; ++_ext) {
      auto ext = static_cast<extent_types_t>(_ext);
      const auto& extent_sizes = get_by_ext(stats.dirty_sizes_by_ext, ext);

      if (is_data_type(ext)) {
        data_sizes.add(extent_sizes);
      } else if (is_logical_metadata_type(ext)) {
        mdat_sizes.add(extent_sizes);
      } else if (is_physical_type(ext)) {
        phys_sizes.add(extent_sizes);
      }
    }
    oss << "\n  data" << data_sizes
        << "\n  mdat" << mdat_sizes
        << "\n  phys" << phys_sizes;

    oss << "\ndirty io: "
        << dirty_io_stats_printer_t{seconds, ret.dirty_io};
    for (uint8_t _src=0; _src<TRANSACTION_TYPE_MAX; ++_src) {
      auto src = static_cast<transaction_type_t>(_src);
      const auto& trans_io_per_src = get_by_src(trans_io_by_src, src);
      if (trans_io_per_src.is_empty()) {
        continue;
      }
      dirty_io_stats_t data_io;
      dirty_io_stats_t mdat_io;
      dirty_io_stats_t phys_io;
      const auto& io_by_ext = get_by_src(_trans_io_by_src_ext, src);
      for (uint8_t _ext=0; _ext<EXTENT_TYPES_MAX; ++_ext) {
        auto ext = static_cast<extent_types_t>(_ext);
        const auto& extent_io = get_by_ext(io_by_ext, ext);
        if (is_data_type(ext)) {
          data_io.add(extent_io);
        } else if (is_logical_metadata_type(ext)) {
          mdat_io.add(extent_io);
        } else if (is_physical_type(ext)) {
          phys_io.add(extent_io);
        }
      }
      oss << "\n  " << src << ": "
          << dirty_io_stats_printer_t{seconds, trans_io_per_src}
          << "\n    data: "
          << dirty_io_stats_printer_t{seconds, data_io}
          << "\n    mdat: "
          << dirty_io_stats_printer_t{seconds, mdat_io}
          << "\n    phys: "
          << dirty_io_stats_printer_t{seconds, phys_io};
    }

    constexpr const char* dfmt = "{:.2f}";
    rewrite_stats_t _trim_rewrites = stats.trim_rewrites;
    _trim_rewrites.minus(last_trim_rewrites);
    rewrite_stats_t _reclaim_rewrites = stats.reclaim_rewrites;
    _reclaim_rewrites.minus(last_reclaim_rewrites);
    oss << "\nrewrite trim ndirty="
        << fmt::format(dfmt, _trim_rewrites.num_n_dirty/seconds)
        << "ps, dirty="
        << fmt::format(dfmt, _trim_rewrites.num_dirty/seconds)
        << "ps, dversion="
        << fmt::format(dfmt, _trim_rewrites.get_avg_version())
        << "; reclaim ndirty="
        << fmt::format(dfmt, _reclaim_rewrites.num_n_dirty/seconds)
        << "ps, dirty="
        << fmt::format(dfmt, _reclaim_rewrites.num_dirty/seconds)
        << "ps, dversion="
        << fmt::format(dfmt, _reclaim_rewrites.get_avg_version());

    oss << "\ncache total"
        << cache_size_stats_t{extents_index.get_bytes(), extents_index.size()};

    counter_by_src_t<counter_by_extent_t<cache_access_stats_t> >
      _access_by_src_ext = stats.access_by_src_ext;
    counter_by_src_t<cache_access_stats_t> access_by_src;
    for (uint8_t _src=0; _src<TRANSACTION_TYPE_MAX; ++_src) {
      auto src = static_cast<transaction_type_t>(_src);
      cache_access_stats_t& trans_access = get_by_src(access_by_src, src);
      auto& access_by_ext = get_by_src(_access_by_src_ext, src);
      const auto& last_access_by_ext = get_by_src(last_access_by_src_ext, src);
      for (uint8_t _ext=0; _ext<EXTENT_TYPES_MAX; ++_ext) {
        auto ext = static_cast<extent_types_t>(_ext);
        cache_access_stats_t& extent_access = get_by_ext(access_by_ext, ext);
        const auto& last_extent_access = get_by_ext(last_access_by_ext, ext);
        extent_access.minus(last_extent_access);
        trans_access.add(extent_access);
      }
    }
    oss << "\naccess: total"
        << cache_access_stats_printer_t{seconds, ret.access};
    for (uint8_t _src=0; _src<TRANSACTION_TYPE_MAX; ++_src) {
      auto src = static_cast<transaction_type_t>(_src);
      const auto& trans_access = get_by_src(access_by_src, src);
      if (trans_access.is_empty()) {
        continue;
      }
      cache_access_stats_t data_access;
      cache_access_stats_t mdat_access;
      cache_access_stats_t phys_access;
      const auto& access_by_ext = get_by_src(_access_by_src_ext, src);
      for (uint8_t _ext=0; _ext<EXTENT_TYPES_MAX; ++_ext) {
        auto ext = static_cast<extent_types_t>(_ext);
        const auto& extent_access = get_by_ext(access_by_ext, ext);
        if (is_data_type(ext)) {
          data_access.add(extent_access);
        } else if (is_logical_metadata_type(ext)) {
          mdat_access.add(extent_access);
        } else if (is_physical_type(ext)) {
          phys_access.add(extent_access);
        }
      }
      oss << "\n  " << src << ": "
          << cache_access_stats_printer_t{seconds, trans_access}
          << "\n    data"
          << cache_access_stats_printer_t{seconds, data_access}
          << "\n    mdat"
          << cache_access_stats_printer_t{seconds, mdat_access}
          << "\n    phys"
          << cache_access_stats_printer_t{seconds, phys_access};
    }

    INFO("{}", oss.str());

    last_dirty_io_by_src_ext = stats.dirty_io_by_src_ext;
    last_trim_rewrites = stats.trim_rewrites;
    last_reclaim_rewrites = stats.reclaim_rewrites;
    last_cache_absent_by_src = stats.cache_absent_by_src;
    last_access_by_src_ext = stats.access_by_src_ext;
  }

  last_dirty_io = stats.dirty_io;
  last_access = stats.access;

  return ret;
}

}
