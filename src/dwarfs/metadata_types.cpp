/* vim:set ts=2 sw=2 sts=2 et: */
/**
 * \author     Marcus Holland-Moritz (github@mhxnet.de)
 * \copyright  Copyright (c) Marcus Holland-Moritz
 *
 * This file is part of dwarfs.
 *
 * dwarfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dwarfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dwarfs.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <numeric>
#include <queue>

#include <fmt/format.h>

#include "dwarfs/error.h"
#include "dwarfs/logger.h"
#include "dwarfs/metadata_types.h"
#include "dwarfs/overloaded.h"

#include "dwarfs/gen-cpp2/metadata_types_custom_protocol.h"

namespace dwarfs {

namespace {

std::vector<thrift::metadata::directory>
unpack_directories(logger& lgr, global_metadata::Meta const* meta) {
  std::vector<thrift::metadata::directory> directories;

  if (auto opts = meta->options(); opts and opts->packed_directories()) {
    LOG_PROXY(debug_logger_policy, lgr);

    auto ti = LOG_TIMED_DEBUG;

    auto dirent = *meta->dir_entries();
    auto metadir = meta->directories();

    directories.resize(metadir.size());

    // delta-decode first entries first
    directories[0].first_entry() = metadir[0].first_entry();

    for (size_t i = 1; i < directories.size(); ++i) {
      directories[i].first_entry() =
          directories[i - 1].first_entry().value() + metadir[i].first_entry();
    }

    // then traverse to recover parent entries
    std::queue<uint32_t> queue;
    queue.push(0);

    while (!queue.empty()) {
      auto parent = queue.front();
      queue.pop();

      auto p_ino = dirent[parent].inode_num();

      auto beg = directories[p_ino].first_entry().value();
      auto end = directories[p_ino + 1].first_entry().value();

      for (auto e = beg; e < end; ++e) {
        if (auto e_ino = dirent[e].inode_num();
            e_ino < (directories.size() - 1)) {
          directories[e_ino].parent_entry() = parent;
          queue.push(e);
        }
      }
    }

    ti << "unpacked directories table";
  }

  return directories;
}

int mode_rank(uint16_t mode) {
  switch (mode & S_IFMT) {
  case S_IFDIR:
    return 0;
  case S_IFLNK:
    return 1;
  case S_IFREG:
    return 2;
  case S_IFBLK:
  case S_IFCHR:
    return 3;
  default:
    return 4;
  }
}

void check_empty_tables(global_metadata::Meta const* meta) {
  if (meta->inodes().empty()) {
    DWARFS_THROW(runtime_error, "empty inodes table");
  }

  if (meta->directories().empty()) {
    DWARFS_THROW(runtime_error, "empty directories table");
  }

  if (meta->chunk_table().empty()) {
    DWARFS_THROW(runtime_error, "empty chunk_table table");
  }

  if (auto de = meta->dir_entries()) {
    if (de->empty()) {
      DWARFS_THROW(runtime_error, "empty dir_entries table");
    }
  } else {
    if (meta->entry_table_v2_2().empty()) {
      DWARFS_THROW(runtime_error, "empty entry_table_v2_2 table");
    }
  }

  if (meta->modes().empty()) {
    DWARFS_THROW(runtime_error, "empty modes table");
  }
}

void check_index_range(global_metadata::Meta const* meta) {
  auto num_modes = meta->modes().size();
  auto num_uids = meta->uids().size();
  auto num_gids = meta->gids().size();
  auto num_names = meta->names().size();
  auto num_inodes = meta->inodes().size();
  bool v2_2 = !static_cast<bool>(meta->dir_entries());

  if (num_modes >= std::numeric_limits<uint16_t>::max()) {
    DWARFS_THROW(runtime_error, "invalid number of modes");
  }

  if (num_uids >= std::numeric_limits<uint16_t>::max()) {
    DWARFS_THROW(runtime_error, "invalid number of uids");
  }

  if (num_gids >= std::numeric_limits<uint16_t>::max()) {
    DWARFS_THROW(runtime_error, "invalid number of gids");
  }

  if (num_names >= std::numeric_limits<uint32_t>::max()) {
    DWARFS_THROW(runtime_error, "invalid number of names");
  }

  if (num_inodes >= std::numeric_limits<uint32_t>::max()) {
    DWARFS_THROW(runtime_error, "invalid number of inodes");
  }

  for (auto ino : meta->inodes()) {
    if (ino.mode_index() >= num_modes) {
      DWARFS_THROW(runtime_error, "mode_index out of range");
    }
    if (auto i = ino.owner_index(); i >= num_uids && i > 0) {
      DWARFS_THROW(runtime_error, "owner_index out of range");
    }
    if (auto i = ino.group_index(); i >= num_gids && i > 0) {
      DWARFS_THROW(runtime_error, "group_index out of range");
    }
    if (v2_2) {
      if (auto i = ino.name_index_v2_2(); i >= num_names && i > 0) {
        DWARFS_THROW(runtime_error, "name_index_v2_2 out of range");
      }
    }
  }

  if (auto dep = meta->dir_entries()) {
    if (dep->size() >= std::numeric_limits<uint32_t>::max()) {
      DWARFS_THROW(runtime_error, "invalid number of dir_entries");
    }

    if (auto cn = meta->compact_names()) {
      num_names = cn->index().size();
      if (!cn->packed_index()) {
        if (num_names == 0) {
          DWARFS_THROW(runtime_error, "empty compact_names index");
        }
        --num_names;
      }
    }

    for (auto de : *dep) {
      if (auto i = de.name_index(); i >= num_names && i > 0) {
        DWARFS_THROW(runtime_error, "name_index out of range");
      }
      if (auto i = de.inode_num(); i >= num_inodes) {
        DWARFS_THROW(runtime_error, "inode_num out of range");
      }
    }
  } else {
    if (meta->entry_table_v2_2().size() >=
        std::numeric_limits<uint32_t>::max()) {
      DWARFS_THROW(runtime_error, "invalid number of entries");
    }

    for (auto ent : meta->entry_table_v2_2()) {
      if (ent >= num_inodes) {
        DWARFS_THROW(runtime_error, "entry_table_v2_2 value out of range");
      }
    }
  }
}

void check_packed_tables(global_metadata::Meta const* meta) {
  if (meta->directories().size() >= std::numeric_limits<uint32_t>::max()) {
    DWARFS_THROW(runtime_error, "invalid number of directories");
  }

  if (meta->chunk_table().size() >= std::numeric_limits<uint32_t>::max()) {
    DWARFS_THROW(runtime_error, "invalid number of chunk_table entries");
  }

  if (auto opt = meta->options(); opt and opt->packed_directories()) {
    if (std::any_of(meta->directories().begin(), meta->directories().end(),
                    [](auto i) { return i.parent_entry() != 0; })) {
      DWARFS_THROW(runtime_error, "parent_entry set in packed directory");
    }
    if (std::accumulate(meta->directories().begin(), meta->directories().end(),
                        static_cast<size_t>(0), [](auto n, auto d) {
                          return n + d.first_entry();
                        }) != meta->dir_entries()->size()) {
      DWARFS_THROW(runtime_error,
                   "first_entry inconsistency in packed directories");
    }
  } else {
    size_t num_entries = meta->dir_entries() ? meta->dir_entries()->size()
                                             : meta->inodes().size();

    if (!std::is_sorted(
            meta->directories().begin(), meta->directories().end(),
            [](auto a, auto b) { return a.first_entry() < b.first_entry(); })) {
      DWARFS_THROW(runtime_error, "first_entry inconsistency");
    }

    for (auto d : meta->directories()) {
      if (auto i = d.first_entry(); i > num_entries) {
        DWARFS_THROW(runtime_error, "first_entry out of range");
      }
      if (auto i = d.parent_entry(); i >= num_entries) {
        DWARFS_THROW(runtime_error, "parent_entry out of range");
      }
    }
  }

  if (auto opt = meta->options(); opt and opt->packed_chunk_table()) {
    if (std::accumulate(meta->chunk_table().begin(), meta->chunk_table().end(),
                        static_cast<size_t>(0)) != meta->chunks().size()) {
      DWARFS_THROW(runtime_error, "packed chunk_table inconsistency");
    }
  } else {
    if (!std::is_sorted(meta->chunk_table().begin(),
                        meta->chunk_table().end()) or
        meta->chunk_table().back() != meta->chunks().size()) {
      DWARFS_THROW(runtime_error, "chunk_table inconsistency");
    }
  }
}

void check_compact_strings(
    ::apache::thrift::frozen::View<thrift::metadata::string_table> v,
    size_t expected_num, size_t max_item_len, std::string const& what) {
  size_t index_size = v.index().size();

  if (!v.packed_index() && index_size > 0) {
    --index_size;
  }

  if (index_size != expected_num) {
    DWARFS_THROW(runtime_error, "unexpected number of compact " + what);
  }

  size_t expected_data_size = 0;
  size_t longest_item_len = 0;
  if (!v.index().empty()) {
    if (v.packed_index()) {
      expected_data_size =
          std::accumulate(v.index().begin(), v.index().end(), 0);
      longest_item_len = *std::max_element(v.index().begin(), v.index().end());
    } else {
      expected_data_size = v.index().back();
      if (!std::is_sorted(v.index().begin(), v.index().end())) {
        DWARFS_THROW(runtime_error, "inconsistent index for compact " + what);
      }
    }
  }

  if (v.buffer().size() != expected_data_size) {
    DWARFS_THROW(runtime_error, "data size mismatch for compact " + what);
  }

  if (longest_item_len > max_item_len) {
    DWARFS_THROW(runtime_error,
                 fmt::format("invalid item length in compact {0}: {1} > {2}",
                             what, longest_item_len, max_item_len));
  }
}

void check_plain_strings(
    ::apache::thrift::frozen::View<std::vector<std::string>> v,
    size_t expected_num, size_t max_item_len, std::string const& what) {
  if (v.size() != expected_num) {
    DWARFS_THROW(runtime_error, "unexpected number of " + what);
  }

  size_t total_size = 0;

  for (auto s : v) {
    if (s.size() > max_item_len) {
      DWARFS_THROW(runtime_error, "unexpectedly long item in " + what);
    }
    total_size += s.size();
  }

  if (!v.empty()) {
    if (total_size != static_cast<size_t>(v.back().end() - v.front().begin())) {
      DWARFS_THROW(runtime_error, "unexpectedly data size in " + what);
    }
  }
}

void check_string_tables(global_metadata::Meta const* meta) {
  size_t num_names = 0;
  if (auto dep = meta->dir_entries()) {
    if (dep->size() > 1) {
      num_names = std::max_element(dep->begin(), dep->end(),
                                   [](auto const& a, auto const& b) {
                                     return a.name_index() < b.name_index();
                                   })
                      ->name_index() +
                  1;
    }
  } else {
    if (meta->inodes().size() > 1) {
      num_names =
          std::max_element(meta->inodes().begin(), meta->inodes().end(),
                           [](auto const& a, auto const& b) {
                             return a.name_index_v2_2() < b.name_index_v2_2();
                           })
              ->name_index_v2_2() +
          1;
    }
  }

  // max name length is usually 255, but fsst compression, in the worst
  // case, will use 2 bytes per input byte...
  constexpr size_t max_name_len = 512;
  constexpr size_t max_symlink_len = 4096;

  if (auto cn = meta->compact_names()) {
    check_compact_strings(*cn, num_names, max_name_len, "names");
  } else {
    check_plain_strings(meta->names(), num_names, max_name_len, "names");
  }

  size_t num_symlink_strings = 0;
  if (!meta->symlink_table().empty()) {
    num_symlink_strings = *std::max_element(meta->symlink_table().begin(),
                                            meta->symlink_table().end()) +
                          1;
  }

  if (auto cs = meta->compact_symlinks()) {
    check_compact_strings(*cs, num_symlink_strings, max_symlink_len,
                          "symlink strings");
  } else {
    check_plain_strings(meta->symlinks(), num_symlink_strings, max_symlink_len,
                        "symlink strings");
  }
}

void check_chunks(global_metadata::Meta const* meta) {
  auto block_size = meta->block_size();

  if (block_size == 0 || (block_size & (block_size - 1))) {
    DWARFS_THROW(runtime_error, "invalid block size");
  }

  if (meta->chunks().size() >= std::numeric_limits<uint32_t>::max()) {
    DWARFS_THROW(runtime_error, "invalid number of chunks");
  }

  for (auto c : meta->chunks()) {
    if (c.offset() >= block_size || c.size() > block_size) {
      DWARFS_THROW(runtime_error, "chunk offset/size out of range");
    }
    if (c.offset() + c.size() > block_size) {
      DWARFS_THROW(runtime_error, "chunk end outside of block");
    }
  }
}

std::array<size_t, 6> check_partitioning(global_metadata::Meta const* meta) {
  std::array<size_t, 6> offsets;

  for (int r = 0; r < static_cast<int>(offsets.size()); ++r) {
    if (auto dep = meta->dir_entries()) {
      auto pred = [r, modes = meta->modes()](auto ino) {
        return mode_rank(modes[ino.mode_index()]) < r;
      };
      auto inodes = meta->inodes();

      if (!std::is_partitioned(inodes.begin(), inodes.end(), pred)) {
        DWARFS_THROW(runtime_error, "inode table inconsistency");
      }

      offsets[r] = std::distance(
          inodes.begin(),
          std::partition_point(inodes.begin(), inodes.end(), pred));
    } else {
      auto pred = [r, modes = meta->modes(),
                   inodes = meta->inodes()](auto ent) {
        return mode_rank(modes[inodes[ent].mode_index()]) < r;
      };
      auto entries = meta->entry_table_v2_2();

      if (!std::is_partitioned(entries.begin(), entries.end(), pred)) {
        DWARFS_THROW(runtime_error, "entry_table_v2_2 inconsistency");
      }

      offsets[r] = std::distance(
          entries.begin(),
          std::partition_point(entries.begin(), entries.end(), pred));
    }
  }

  return offsets;
}

global_metadata::Meta const*
check_metadata(logger& lgr, global_metadata::Meta const* meta, bool check) {
  if (check) {
    LOG_PROXY(debug_logger_policy, lgr);

    auto ti = LOG_TIMED_DEBUG;

    ti << "check metadata consistency";

    check_empty_tables(meta);
    check_index_range(meta);
    check_packed_tables(meta);
    check_string_tables(meta);
    check_chunks(meta);
    auto offsets = check_partitioning(meta);

    auto num_dir = meta->directories().size() - 1;
    auto num_lnk = meta->symlink_table().size();
    auto num_reg_unique = meta->chunk_table().size() - 1;
    size_t num_reg_shared = 0;

    if (auto sfp = meta->shared_files_table()) {
      if (meta->options()->packed_shared_files_table()) {
        num_reg_shared =
            std::accumulate(sfp->begin(), sfp->end(), 2 * sfp->size());
        num_reg_unique -= sfp->size();
      } else {
        if (!std::is_sorted(sfp->begin(), sfp->end())) {
          DWARFS_THROW(runtime_error,
                       "unpacked shared_files_table is not sorted");
        }
        num_reg_shared = sfp->size();
        if (!sfp->empty()) {
          num_reg_unique -= sfp->back() + 1;
        }
      }
    }

    size_t num_dev = meta->devices() ? meta->devices()->size() : 0;

    if (num_dir != offsets[1]) {
      DWARFS_THROW(runtime_error, "wrong number of directories");
    }

    if (num_lnk != offsets[2] - offsets[1]) {
      DWARFS_THROW(runtime_error, "wrong number of links");
    }

    if (num_reg_unique + num_reg_shared != offsets[3] - offsets[2]) {
      DWARFS_THROW(runtime_error, "wrong number of files");
    }

    if (num_dev != offsets[4] - offsets[3]) {
      DWARFS_THROW(runtime_error, "wrong number of devices");
    }

    if (!meta->dir_entries()) {
      for (auto ino : meta->inodes()) {
        auto mode = meta->modes()[ino.mode_index()];
        auto i = ino.inode_v2_2();
        int base = mode_rank(mode);

        if (i < offsets[base] ||
            (i >= offsets[base + 1] && i > offsets[base])) {
          DWARFS_THROW(runtime_error, "inode_v2_2 out of range");
        }
      }
    }
  }

  return meta;
}

} // namespace

global_metadata::global_metadata(logger& lgr, Meta const* meta,
                                 bool check_consistency)
    : meta_{check_metadata(lgr, meta, check_consistency)}
    , directories_storage_{unpack_directories(lgr, meta_)}
    , directories_{directories_storage_.empty() ? nullptr
                                                : directories_storage_.data()}
    , names_{meta_->compact_names()
                 ? string_table(lgr, "names", *meta_->compact_names())
                 : string_table(meta_->names())} {}

uint32_t global_metadata::first_dir_entry(uint32_t ino) const {
  return directories_ ? directories_[ino].first_entry().value()
                      : meta_->directories()[ino].first_entry();
}

uint32_t global_metadata::parent_dir_entry(uint32_t ino) const {
  return directories_ ? directories_[ino].parent_entry().value()
                      : meta_->directories()[ino].parent_entry();
}

uint16_t inode_view::mode() const { return meta_->modes()[mode_index()]; }

uint16_t inode_view::getuid() const { return meta_->uids()[owner_index()]; }

uint16_t inode_view::getgid() const { return meta_->gids()[group_index()]; }

// TODO: pretty certain some of this stuff can be simplified

std::string dir_entry_view::name() const {
  return std::visit(overloaded{
                        [this](DirEntryView const& dev) {
                          return g_->names()[dev.name_index()];
                        },
                        [this](InodeView const& iv) {
                          return std::string(
                              g_->meta()->names()[iv.name_index_v2_2()]);
                        },
                    },
                    v_);
}

inode_view dir_entry_view::inode() const {
  return std::visit(overloaded{
                        [this](DirEntryView const& dev) {
                          return inode_view(
                              g_->meta()->inodes()[dev.inode_num()],
                              dev.inode_num(), g_->meta());
                        },
                        [this](InodeView const& iv) {
                          return inode_view(iv, iv.inode_v2_2(), g_->meta());
                        },
                    },
                    v_);
}

bool dir_entry_view::is_root() const {
  return std::visit(
      overloaded{
          [](DirEntryView const& dev) { return dev.inode_num() == 0; },
          [](InodeView const& iv) { return iv.inode_v2_2() == 0; },
      },
      v_);
}

/**
 * We need a parent index if the dir_entry_view is for a file. For
 * directories, the parent can be determined via the directory's
 * inode, but for files, this isn't possible.
 */

dir_entry_view
dir_entry_view::from_dir_entry_index(uint32_t self_index, uint32_t parent_index,
                                     global_metadata const* g) {
  auto meta = g->meta();

  if (auto de = meta->dir_entries()) {
    DWARFS_CHECK(self_index < de->size(), "self_index out of range");
    DWARFS_CHECK(parent_index < de->size(), "parent_index out of range");

    auto dev = (*de)[self_index];

    return dir_entry_view(dev, self_index, parent_index, g);
  }

  DWARFS_CHECK(self_index < meta->inodes().size(), "self_index out of range");
  DWARFS_CHECK(parent_index < meta->inodes().size(), "self_index out of range");

  auto iv = meta->inodes()[self_index];

  return dir_entry_view(iv, self_index, parent_index, g);
}

dir_entry_view dir_entry_view::from_dir_entry_index(uint32_t self_index,
                                                    global_metadata const* g) {
  auto meta = g->meta();

  if (auto de = meta->dir_entries()) {
    DWARFS_CHECK(self_index < de->size(), "self_index out of range");
    auto dev = (*de)[self_index];
    DWARFS_CHECK(dev.inode_num() < meta->directories().size(),
                 "self_index inode out of range");
    return dir_entry_view(dev, self_index, g->parent_dir_entry(dev.inode_num()),
                          g);
  }

  DWARFS_CHECK(self_index < meta->inodes().size(), "self_index out of range");
  auto iv = meta->inodes()[self_index];

  DWARFS_CHECK(iv.inode_v2_2() < meta->directories().size(),
               "parent_index out of range");
  return dir_entry_view(
      iv, self_index,
      meta->entry_table_v2_2()[meta->directories()[iv.inode_v2_2()]
                                   .parent_entry()],
      g);
}

std::optional<dir_entry_view> dir_entry_view::parent() const {
  if (is_root()) {
    return std::nullopt;
  }

  return from_dir_entry_index(parent_index_, g_);
}

std::string dir_entry_view::name(uint32_t index, global_metadata const* g) {
  if (auto de = g->meta()->dir_entries()) {
    DWARFS_CHECK(index < de->size(), "index out of range");
    auto dev = (*de)[index];
    return g->names()[dev.name_index()];
  }

  DWARFS_CHECK(index < g->meta()->inodes().size(), "index out of range");
  auto iv = g->meta()->inodes()[index];
  return std::string(g->meta()->names()[iv.name_index_v2_2()]);
}

inode_view dir_entry_view::inode(uint32_t index, global_metadata const* g) {
  if (auto de = g->meta()->dir_entries()) {
    DWARFS_CHECK(index < de->size(), "index out of range");
    auto dev = (*de)[index];
    return inode_view(g->meta()->inodes()[dev.inode_num()], dev.inode_num(),
                      g->meta());
  }

  DWARFS_CHECK(index < g->meta()->inodes().size(), "index out of range");
  auto iv = g->meta()->inodes()[index];
  return inode_view(iv, iv.inode_v2_2(), g->meta());
}

std::string dir_entry_view::path() const {
  std::string p;
  append_path_to(p);
  return p;
}

void dir_entry_view::append_path_to(std::string& s) const {
  if (auto p = parent()) {
    if (!p->is_root()) {
      p->append_path_to(s);
      s += '/';
    }
  }
  if (!is_root()) {
    s += name();
  }
}

uint32_t directory_view::first_entry(uint32_t ino) const {
  return g_->first_dir_entry(ino);
}

uint32_t directory_view::parent_entry(uint32_t ino) const {
  return g_->parent_dir_entry(ino);
}

uint32_t directory_view::entry_count() const {
  return first_entry(inode_ + 1) - first_entry();
}

boost::integer_range<uint32_t> directory_view::entry_range() const {
  return boost::irange(first_entry(), first_entry(inode_ + 1));
}

uint32_t directory_view::parent_inode() const {
  if (inode_ == 0) {
    return 0;
  }

  auto ent = parent_entry(inode_);

  if (auto e = g_->meta()->dir_entries()) {
    ent = (*e)[ent].inode_num();
  }

  return ent;
}

} // namespace dwarfs
