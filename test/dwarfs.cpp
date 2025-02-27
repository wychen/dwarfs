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
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include <sys/statvfs.h>

#include <gtest/gtest.h>

#include <fmt/format.h>

#include "dwarfs/block_compressor.h"
#include "dwarfs/builtin_script.h"
#include "dwarfs/entry.h"
#include "dwarfs/filesystem_v2.h"
#include "dwarfs/filesystem_writer.h"
#include "dwarfs/logger.h"
#include "dwarfs/mmif.h"
#include "dwarfs/options.h"
#include "dwarfs/progress.h"
#include "dwarfs/scanner.h"

#include "filter_test_data.h"
#include "loremipsum.h"
#include "mmap_mock.h"
#include "test_helpers.h"

using namespace dwarfs;

namespace {

std::string const default_file_hash_algo{"xxh3-128"};

std::string
build_dwarfs(logger& lgr, std::shared_ptr<test::os_access_mock> input,
             std::string const& compression,
             block_manager::config const& cfg = block_manager::config(),
             scanner_options const& options = scanner_options(),
             progress* prog = nullptr, std::shared_ptr<script> scr = nullptr,
             std::optional<std::span<std::filesystem::path const>> input_list =
                 std::nullopt) {
  // force multithreading
  worker_group wg("worker", 4);

  scanner s(lgr, wg, cfg, entry_factory::create(), input, scr, options);

  std::ostringstream oss;
  std::unique_ptr<progress> local_prog;
  if (!prog) {
    local_prog = std::make_unique<progress>([](const progress&, bool) {}, 1000);
    prog = local_prog.get();
  }

  block_compressor bc(compression);
  filesystem_writer fsw(oss, lgr, wg, *prog, bc);

  s.scan(fsw, "", *prog, input_list);

  return oss.str();
}

void basic_end_to_end_test(std::string const& compressor,
                           unsigned block_size_bits, file_order_mode file_order,
                           bool with_devices, bool with_specials, bool set_uid,
                           bool set_gid, bool set_time, bool keep_all_times,
                           bool enable_nlink, bool pack_chunk_table,
                           bool pack_directories, bool pack_shared_files_table,
                           bool pack_names, bool pack_names_index,
                           bool pack_symlinks, bool pack_symlinks_index,
                           bool plain_names_table, bool plain_symlinks_table,
                           bool access_fail,
                           std::optional<std::string> file_hash_algo) {
  block_manager::config cfg;
  scanner_options options;

  cfg.blockhash_window_size = 10;
  cfg.block_size_bits = block_size_bits;

  options.file_order.mode = file_order;
  options.file_hash_algorithm = file_hash_algo;
  options.with_devices = with_devices;
  options.with_specials = with_specials;
  options.inode.with_similarity = file_order == file_order_mode::SIMILARITY;
  options.inode.with_nilsimsa = file_order == file_order_mode::NILSIMSA;
  options.keep_all_times = keep_all_times;
  options.pack_chunk_table = pack_chunk_table;
  options.pack_directories = pack_directories;
  options.pack_shared_files_table = pack_shared_files_table;
  options.pack_names = pack_names;
  options.pack_names_index = pack_names_index;
  options.pack_symlinks = pack_symlinks;
  options.pack_symlinks_index = pack_symlinks_index;
  options.force_pack_string_tables = true;
  options.plain_names_table = plain_names_table;
  options.plain_symlinks_table = plain_symlinks_table;

  if (set_uid) {
    options.uid = 0;
  }

  if (set_gid) {
    options.gid = 0;
  }

  if (set_time) {
    options.timestamp = 4711;
  }

  std::ostringstream logss;
  stream_logger lgr(logss); // TODO: mock
  lgr.set_policy<prod_logger_policy>();

  auto input = test::os_access_mock::create_test_instance();

  if (access_fail) {
    input->set_access_fail("/somedir/ipsum.py");
  }

  auto prog = progress([](const progress&, bool) {}, 1000);

  std::shared_ptr<script> scr;
  if (file_order == file_order_mode::SCRIPT) {
    scr = std::make_shared<test::script_mock>();
  }

  auto fsimage = build_dwarfs(lgr, input, compressor, cfg, options, &prog, scr);
  auto image_size = fsimage.size();
  auto mm = std::make_shared<test::mmap_mock>(std::move(fsimage));

  bool similarity =
      options.inode.with_similarity || options.inode.with_nilsimsa;

  size_t const num_fail_empty = access_fail ? 1 : 0;

  EXPECT_EQ(8, prog.files_found);
  EXPECT_EQ(8, prog.files_scanned);
  EXPECT_EQ(2, prog.dirs_found);
  EXPECT_EQ(2, prog.dirs_scanned);
  EXPECT_EQ(2, prog.symlinks_found);
  EXPECT_EQ(2, prog.symlinks_scanned);
  EXPECT_EQ(2 * with_devices + with_specials, prog.specials_found);
  EXPECT_EQ(file_hash_algo ? 3 + num_fail_empty : 0, prog.duplicate_files);
  EXPECT_EQ(1, prog.hardlinks);
  EXPECT_GE(prog.block_count, 1);
  EXPECT_GE(prog.chunk_count, 100);
  EXPECT_EQ(7 - prog.duplicate_files, prog.inodes_scanned);
  EXPECT_EQ(file_hash_algo ? 4 - num_fail_empty : 7, prog.inodes_written);
  EXPECT_EQ(prog.files_found - prog.duplicate_files - prog.hardlinks,
            prog.inodes_written);
  EXPECT_EQ(prog.block_count, prog.blocks_written);
  EXPECT_EQ(num_fail_empty, prog.errors);
  EXPECT_EQ(access_fail ? 2046934 : 2056934, prog.original_size);
  EXPECT_EQ(23456, prog.hardlink_size);
  EXPECT_EQ(file_hash_algo ? 23456 : 0, prog.saved_by_deduplication);
  EXPECT_GE(prog.saved_by_segmentation, block_size_bits == 12 ? 0 : 1000000);
  EXPECT_EQ(prog.original_size -
                (prog.saved_by_deduplication + prog.saved_by_segmentation +
                 prog.symlink_size),
            prog.filesystem_size);
  EXPECT_EQ(prog.similarity_scans, similarity ? prog.inodes_scanned.load() : 0);
  EXPECT_EQ(prog.similarity_bytes,
            similarity ? prog.original_size -
                             (prog.saved_by_deduplication + prog.symlink_size)
                       : 0);
  EXPECT_EQ(prog.hash_scans, file_hash_algo ? 5 + num_fail_empty : 0);
  EXPECT_EQ(prog.hash_bytes, file_hash_algo ? 46912 : 0);
  EXPECT_EQ(image_size, prog.compressed_size);

  filesystem_options opts;
  opts.block_cache.max_bytes = 1 << 20;
  opts.metadata.enable_nlink = enable_nlink;
  opts.metadata.check_consistency = true;

  filesystem_v2 fs(lgr, mm, opts);

  // fs.dump(std::cerr, 9);

  struct ::statvfs vfsbuf;
  fs.statvfs(&vfsbuf);

  EXPECT_EQ(1 << block_size_bits, vfsbuf.f_bsize);
  EXPECT_EQ(1, vfsbuf.f_frsize);
  if (enable_nlink) {
    EXPECT_EQ(access_fail ? 2046934 : 2056934, vfsbuf.f_blocks);
  } else {
    EXPECT_EQ(access_fail ? 2070390 : 2080390, vfsbuf.f_blocks);
  }
  EXPECT_EQ(11 + 2 * with_devices + with_specials, vfsbuf.f_files);
  EXPECT_EQ(ST_RDONLY, vfsbuf.f_flag);
  EXPECT_GT(vfsbuf.f_namemax, 0);

  std::ostringstream dumpss;

  fs.dump(dumpss, 9);

  EXPECT_GT(dumpss.str().size(), 1000) << dumpss.str();

  auto entry = fs.find("/foo.pl");
  struct ::stat st;

  ASSERT_TRUE(entry);
  EXPECT_EQ(fs.getattr(*entry, &st), 0);
  EXPECT_EQ(st.st_size, 23456);
  EXPECT_EQ(st.st_uid, set_uid ? 0 : 1337);
  EXPECT_EQ(st.st_gid, 0);
  EXPECT_EQ(st.st_atime, set_time ? 4711 : keep_all_times ? 4001 : 4002);
  EXPECT_EQ(st.st_mtime, set_time ? 4711 : keep_all_times ? 4002 : 4002);
  EXPECT_EQ(st.st_ctime, set_time ? 4711 : keep_all_times ? 4003 : 4002);

  int inode = fs.open(*entry);
  EXPECT_GE(inode, 0);

  std::vector<char> buf(st.st_size);
  ssize_t rv = fs.read(inode, &buf[0], st.st_size, 0);
  EXPECT_EQ(rv, st.st_size);
  EXPECT_EQ(std::string(buf.begin(), buf.end()), test::loremipsum(st.st_size));

  entry = fs.find("/somelink");

  ASSERT_TRUE(entry);
  EXPECT_EQ(fs.getattr(*entry, &st), 0);
  EXPECT_EQ(st.st_size, 16);
  EXPECT_EQ(st.st_uid, set_uid ? 0 : 1000);
  EXPECT_EQ(st.st_gid, set_gid ? 0 : 100);
  EXPECT_EQ(st.st_rdev, 0);
  EXPECT_EQ(st.st_atime, set_time ? 4711 : keep_all_times ? 2001 : 2002);
  EXPECT_EQ(st.st_mtime, set_time ? 4711 : keep_all_times ? 2002 : 2002);
  EXPECT_EQ(st.st_ctime, set_time ? 4711 : keep_all_times ? 2003 : 2002);

  std::string link;
  EXPECT_EQ(fs.readlink(*entry, &link), 0);
  EXPECT_EQ(link, "somedir/ipsum.py");

  EXPECT_FALSE(fs.find("/somedir/nope"));

  entry = fs.find("/somedir/bad");

  ASSERT_TRUE(entry);
  EXPECT_EQ(fs.getattr(*entry, &st), 0);
  EXPECT_EQ(st.st_size, 6);

  EXPECT_EQ(fs.readlink(*entry, &link), 0);
  EXPECT_EQ(link, "../foo");

  entry = fs.find("/somedir/pipe");

  if (with_specials) {
    ASSERT_TRUE(entry);
    EXPECT_EQ(fs.getattr(*entry, &st), 0);
    EXPECT_EQ(st.st_size, 0);
    EXPECT_EQ(st.st_uid, set_uid ? 0 : 1000);
    EXPECT_EQ(st.st_gid, set_gid ? 0 : 100);
    EXPECT_TRUE(S_ISFIFO(st.st_mode));
    EXPECT_EQ(st.st_rdev, 0);
    EXPECT_EQ(st.st_atime, set_time ? 4711 : keep_all_times ? 8001 : 8002);
    EXPECT_EQ(st.st_mtime, set_time ? 4711 : keep_all_times ? 8002 : 8002);
    EXPECT_EQ(st.st_ctime, set_time ? 4711 : keep_all_times ? 8003 : 8002);
  } else {
    EXPECT_FALSE(entry);
  }

  entry = fs.find("/somedir/null");

  if (with_devices) {
    ASSERT_TRUE(entry);
    EXPECT_EQ(fs.getattr(*entry, &st), 0);
    EXPECT_EQ(st.st_size, 0);
    EXPECT_EQ(st.st_uid, 0);
    EXPECT_EQ(st.st_gid, 0);
    EXPECT_TRUE(S_ISCHR(st.st_mode));
    EXPECT_EQ(st.st_rdev, 259);
  } else {
    EXPECT_FALSE(entry);
  }

  entry = fs.find("/somedir/zero");

  if (with_devices) {
    ASSERT_TRUE(entry);
    EXPECT_EQ(fs.getattr(*entry, &st), 0);
    EXPECT_EQ(st.st_size, 0);
    EXPECT_EQ(st.st_uid, 0);
    EXPECT_EQ(st.st_gid, 0);
    EXPECT_TRUE(S_ISCHR(st.st_mode));
    EXPECT_EQ(st.st_rdev, 261);
    EXPECT_EQ(st.st_atime, set_time         ? 4711
                           : keep_all_times ? 4000010001
                                            : 4000020002);
    EXPECT_EQ(st.st_mtime, set_time         ? 4711
                           : keep_all_times ? 4000020002
                                            : 4000020002);
    EXPECT_EQ(st.st_ctime, set_time         ? 4711
                           : keep_all_times ? 4000030003
                                            : 4000020002);
  } else {
    EXPECT_FALSE(entry);
  }

  entry = fs.find("/");

  ASSERT_TRUE(entry);
  auto dir = fs.opendir(*entry);
  ASSERT_TRUE(dir);
  EXPECT_EQ(10, fs.dirsize(*dir));

  entry = fs.find("/somedir");

  ASSERT_TRUE(entry);
  dir = fs.opendir(*entry);
  ASSERT_TRUE(dir);
  EXPECT_EQ(5 + 2 * with_devices + with_specials, fs.dirsize(*dir));

  std::vector<std::string> names;
  for (size_t i = 0; i < fs.dirsize(*dir); ++i) {
    auto r = fs.readdir(*dir, i);
    ASSERT_TRUE(r);
    auto [view, name] = *r;
    names.emplace_back(name);
  }

  std::vector<std::string> expected{
      ".", "..", "bad", "empty", "ipsum.py",
  };

  if (with_devices) {
    expected.emplace_back("null");
  }

  if (with_specials) {
    expected.emplace_back("pipe");
  }

  if (with_devices) {
    expected.emplace_back("zero");
  }

  EXPECT_EQ(expected, names);

  entry = fs.find("/foo.pl");
  ASSERT_TRUE(entry);

  auto e2 = fs.find("/bar.pl");
  ASSERT_TRUE(e2);

  EXPECT_EQ(entry->inode_num(), e2->inode_num());

  struct ::stat st1, st2;
  ASSERT_EQ(0, fs.getattr(*entry, &st1));
  ASSERT_EQ(0, fs.getattr(*e2, &st2));

  EXPECT_EQ(st1.st_ino, st2.st_ino);
  if (enable_nlink) {
    EXPECT_EQ(2, st1.st_nlink);
    EXPECT_EQ(2, st2.st_nlink);
  }

  entry = fs.find("/");
  ASSERT_TRUE(entry);
  EXPECT_EQ(0, entry->inode_num());
  e2 = fs.find(0);
  ASSERT_TRUE(e2);
  EXPECT_EQ(e2->inode_num(), 0);
  entry = fs.find(0, "baz.pl");
  ASSERT_TRUE(entry);
  EXPECT_GT(entry->inode_num(), 0);
  ASSERT_EQ(0, fs.getattr(*entry, &st1));
  EXPECT_EQ(23456, st1.st_size);
  e2 = fs.find(0, "somedir");
  ASSERT_TRUE(e2);
  ASSERT_EQ(0, fs.getattr(*e2, &st2));
  entry = fs.find(st2.st_ino, "ipsum.py");
  ASSERT_TRUE(entry);
  ASSERT_EQ(0, fs.getattr(*entry, &st1));
  EXPECT_EQ(access_fail ? 0 : 10000, st1.st_size);
  EXPECT_EQ(0, fs.access(*entry, R_OK, 1000, 100));
  entry = fs.find(0, "baz.pl");
  ASSERT_TRUE(entry);
  EXPECT_EQ(set_uid ? EACCES : 0, fs.access(*entry, R_OK, 1337, 0));

  for (auto mp : {&filesystem_v2::walk, &filesystem_v2::walk_data_order}) {
    std::map<std::string, struct ::stat> entries;
    std::vector<int> inodes;

    (fs.*mp)([&](dir_entry_view e) {
      struct ::stat stbuf;
      ASSERT_EQ(0, fs.getattr(e.inode(), &stbuf));
      inodes.push_back(stbuf.st_ino);
      auto path = e.path();
      if (!path.empty()) {
        path = "/" + path;
      }
      EXPECT_TRUE(entries.emplace(path, stbuf).second);
    });

    EXPECT_EQ(entries.size(),
              input->size() + 2 * with_devices + with_specials - 3);

    for (auto const& [p, st] : entries) {
      struct ::stat ref;
      input->lstat(p, &ref);
      EXPECT_EQ(ref.st_mode, st.st_mode) << p;
      EXPECT_EQ(set_uid ? 0 : ref.st_uid, st.st_uid) << p;
      EXPECT_EQ(set_gid ? 0 : ref.st_gid, st.st_gid) << p;
      if (!S_ISDIR(st.st_mode)) {
        if (input->access(p, R_OK) == 0) {
          EXPECT_EQ(ref.st_size, st.st_size) << p;
        } else {
          EXPECT_EQ(0, st.st_size) << p;
        }
      }
    }
  }

  auto dyn = fs.metadata_as_dynamic();

  EXPECT_TRUE(dyn.isObject());

  auto json = fs.serialize_metadata_as_json(true);

  EXPECT_GT(json.size(), 1000) << json;

  json = fs.serialize_metadata_as_json(false);

  EXPECT_GT(json.size(), 1000) << json;
}

std::vector<std::string> const compressions{
    "null",
#ifdef DWARFS_HAVE_LIBLZ4
    "lz4",
    "lz4hc:level=4",
#endif
#ifdef DWARFS_HAVE_LIBZSTD
    "zstd:level=1",
#endif
#ifdef DWARFS_HAVE_LIBLZMA
    "lzma:level=1",
#endif
#ifdef DWARFS_HAVE_LIBBROTLI
    "brotli:quality=2",
#endif
};

} // namespace

class compression_test
    : public testing::TestWithParam<std::tuple<
          std::string, unsigned, file_order_mode, std::optional<std::string>>> {
};

class scanner_test : public testing::TestWithParam<
                         std::tuple<bool, bool, bool, bool, bool, bool, bool,
                                    bool, std::optional<std::string>>> {};

class hashing_test : public testing::TestWithParam<std::string> {};

class packing_test : public testing::TestWithParam<
                         std::tuple<bool, bool, bool, bool, bool, bool, bool>> {
};

class plain_tables_test
    : public testing::TestWithParam<std::tuple<bool, bool>> {};

TEST_P(compression_test, end_to_end) {
  auto [compressor, block_size_bits, file_order, file_hash_algo] = GetParam();

  if (compressor.find("lzma") == 0 && block_size_bits < 16) {
    // these are notoriously slow, so just skip them
    return;
  }

  basic_end_to_end_test(compressor, block_size_bits, file_order, true, true,
                        false, false, false, false, false, true, true, true,
                        true, true, true, true, false, false, false,
                        file_hash_algo);
}

TEST_P(scanner_test, end_to_end) {
  auto [with_devices, with_specials, set_uid, set_gid, set_time, keep_all_times,
        enable_nlink, access_fail, file_hash_algo] = GetParam();

  basic_end_to_end_test(
      compressions[0], 15, file_order_mode::NONE, with_devices, with_specials,
      set_uid, set_gid, set_time, keep_all_times, enable_nlink, true, true,
      true, true, true, true, true, false, false, access_fail, file_hash_algo);
}

TEST_P(hashing_test, end_to_end) {
  basic_end_to_end_test(compressions[0], 15, file_order_mode::NONE, true, true,
                        true, true, true, true, true, true, true, true, true,
                        true, true, true, false, false, false, GetParam());
}

TEST_P(packing_test, end_to_end) {
  auto [pack_chunk_table, pack_directories, pack_shared_files_table, pack_names,
        pack_names_index, pack_symlinks, pack_symlinks_index] = GetParam();

  basic_end_to_end_test(compressions[0], 15, file_order_mode::NONE, true, true,
                        false, false, false, false, false, pack_chunk_table,
                        pack_directories, pack_shared_files_table, pack_names,
                        pack_names_index, pack_symlinks, pack_symlinks_index,
                        false, false, false, default_file_hash_algo);
}

TEST_P(plain_tables_test, end_to_end) {
  auto [plain_names_table, plain_symlinks_table] = GetParam();

  basic_end_to_end_test(compressions[0], 15, file_order_mode::NONE, true, true,
                        false, false, false, false, false, false, false, false,
                        false, false, false, false, plain_names_table,
                        plain_symlinks_table, false, default_file_hash_algo);
}

TEST_P(packing_test, regression_empty_fs) {
  auto [pack_chunk_table, pack_directories, pack_shared_files_table, pack_names,
        pack_names_index, pack_symlinks, pack_symlinks_index] = GetParam();

  block_manager::config cfg;
  scanner_options options;

  cfg.blockhash_window_size = 8;
  cfg.block_size_bits = 10;

  options.pack_chunk_table = pack_chunk_table;
  options.pack_directories = pack_directories;
  options.pack_shared_files_table = pack_shared_files_table;
  options.pack_names = pack_names;
  options.pack_names_index = pack_names_index;
  options.pack_symlinks = pack_symlinks;
  options.pack_symlinks_index = pack_symlinks_index;
  options.force_pack_string_tables = true;

  std::ostringstream logss;
  stream_logger lgr(logss); // TODO: mock
  lgr.set_policy<prod_logger_policy>();

  auto input = std::make_shared<test::os_access_mock>();

  input->add_dir("");

  auto mm = std::make_shared<test::mmap_mock>(
      build_dwarfs(lgr, input, "null", cfg, options));

  filesystem_options opts;
  opts.block_cache.max_bytes = 1 << 20;
  opts.metadata.check_consistency = true;

  filesystem_v2 fs(lgr, mm, opts);

  struct ::statvfs vfsbuf;
  fs.statvfs(&vfsbuf);

  EXPECT_EQ(1, vfsbuf.f_files);
  EXPECT_EQ(0, vfsbuf.f_blocks);

  size_t num = 0;

  fs.walk([&](dir_entry_view e) {
    ++num;
    struct ::stat stbuf;
    ASSERT_EQ(0, fs.getattr(e.inode(), &stbuf));
    EXPECT_TRUE(S_ISDIR(stbuf.st_mode));
  });

  EXPECT_EQ(1, num);
}

INSTANTIATE_TEST_SUITE_P(
    dwarfs, compression_test,
    ::testing::Combine(
        ::testing::ValuesIn(compressions), ::testing::Values(12, 15, 20, 28),
        ::testing::Values(file_order_mode::NONE, file_order_mode::PATH,
                          file_order_mode::SCRIPT, file_order_mode::NILSIMSA,
                          file_order_mode::SIMILARITY),
        ::testing::Values(std::nullopt, "xxh3-128")));

INSTANTIATE_TEST_SUITE_P(
    dwarfs, scanner_test,
    ::testing::Combine(::testing::Bool(), ::testing::Bool(), ::testing::Bool(),
                       ::testing::Bool(), ::testing::Bool(), ::testing::Bool(),
                       ::testing::Bool(), ::testing::Bool(),
                       ::testing::Values(std::nullopt, "xxh3-128", "sha512")));

INSTANTIATE_TEST_SUITE_P(dwarfs, hashing_test,
                         ::testing::ValuesIn(checksum::available_algorithms()));

INSTANTIATE_TEST_SUITE_P(
    dwarfs, packing_test,
    ::testing::Combine(::testing::Bool(), ::testing::Bool(), ::testing::Bool(),
                       ::testing::Bool(), ::testing::Bool(), ::testing::Bool(),
                       ::testing::Bool()));

INSTANTIATE_TEST_SUITE_P(dwarfs, plain_tables_test,
                         ::testing::Combine(::testing::Bool(),
                                            ::testing::Bool()));

TEST(block_manager, regression_block_boundary) {
  block_manager::config cfg;

  // make sure we don't actually segment anything
  cfg.blockhash_window_size = 12;
  cfg.block_size_bits = 10;

  filesystem_options opts;
  opts.block_cache.max_bytes = 1 << 20;
  opts.metadata.check_consistency = true;

  std::ostringstream logss;
  stream_logger lgr(logss); // TODO: mock
  lgr.set_policy<prod_logger_policy>();

  std::vector<size_t> fs_sizes;

  for (auto size : {1023, 1024, 1025}) {
    auto input = std::make_shared<test::os_access_mock>();

    input->add_dir("");
    input->add_file("test", size);

    auto fsdata = build_dwarfs(lgr, input, "null", cfg);

    fs_sizes.push_back(fsdata.size());

    auto mm = std::make_shared<test::mmap_mock>(fsdata);

    filesystem_v2 fs(lgr, mm, opts);

    struct ::statvfs vfsbuf;
    fs.statvfs(&vfsbuf);

    EXPECT_EQ(2, vfsbuf.f_files);
    EXPECT_EQ(size, vfsbuf.f_blocks);
  }

  EXPECT_TRUE(std::is_sorted(fs_sizes.begin(), fs_sizes.end()))
      << folly::join(", ", fs_sizes);
}

class compression_regression : public testing::TestWithParam<std::string> {};

TEST_P(compression_regression, github45) {
  auto compressor = GetParam();

  block_manager::config cfg;

  constexpr size_t block_size_bits = 18;
  constexpr size_t file_size = 1 << block_size_bits;

  cfg.blockhash_window_size = 0;
  cfg.block_size_bits = block_size_bits;

  filesystem_options opts;
  opts.block_cache.max_bytes = 1 << 20;
  opts.metadata.check_consistency = true;

  std::ostringstream logss;
  stream_logger lgr(logss); // TODO: mock
  lgr.set_policy<prod_logger_policy>();

  std::independent_bits_engine<std::mt19937_64,
                               std::numeric_limits<uint8_t>::digits, uint8_t>
      rng;

  std::string random;
  random.resize(file_size);
  std::generate(begin(random), end(random), std::ref(rng));

  auto input = std::make_shared<test::os_access_mock>();

  input->add_dir("");
  input->add_file("random", random);
  input->add_file("test", file_size);

  auto fsdata = build_dwarfs(lgr, input, compressor, cfg);

  auto mm = std::make_shared<test::mmap_mock>(fsdata);

  std::stringstream idss;
  filesystem_v2::identify(lgr, mm, idss, 3);

  std::string line;
  std::regex const re("^SECTION num=\\d+, type=BLOCK, compression=(\\w+).*");
  std::set<std::string> compressions;
  while (std::getline(idss, line)) {
    std::smatch m;
    if (std::regex_match(line, m, re)) {
      compressions.emplace(m[1]);
    }
  }

  if (compressor == "null") {
    EXPECT_EQ(1, compressions.size());
  } else {
    EXPECT_EQ(2, compressions.size());
  }
  EXPECT_EQ(1, compressions.count("NONE"));

  filesystem_v2 fs(lgr, mm, opts);

  struct ::statvfs vfsbuf;
  fs.statvfs(&vfsbuf);

  EXPECT_EQ(3, vfsbuf.f_files);
  EXPECT_EQ(2 * file_size, vfsbuf.f_blocks);

  auto check_file = [&](char const* name, std::string const& contents) {
    auto entry = fs.find(name);
    struct ::stat st;

    ASSERT_TRUE(entry);
    EXPECT_EQ(fs.getattr(*entry, &st), 0);
    EXPECT_EQ(st.st_size, file_size);

    int inode = fs.open(*entry);
    EXPECT_GE(inode, 0);

    std::vector<char> buf(st.st_size);
    ssize_t rv = fs.read(inode, &buf[0], st.st_size, 0);
    EXPECT_EQ(rv, st.st_size);
    EXPECT_EQ(std::string(buf.begin(), buf.end()), contents);
  };

  check_file("random", random);
  check_file("test", test::loremipsum(file_size));
}

INSTANTIATE_TEST_SUITE_P(dwarfs, compression_regression,
                         ::testing::ValuesIn(compressions));

class file_scanner
    : public testing::TestWithParam<
          std::tuple<file_order_mode, std::optional<std::string>>> {};

TEST_P(file_scanner, inode_ordering) {
  auto [order_mode, file_hash_algo] = GetParam();

  std::ostringstream logss;
  stream_logger lgr(logss); // TODO: mock
  lgr.set_policy<prod_logger_policy>();

  auto bmcfg = block_manager::config();
  auto opts = scanner_options();

  opts.file_order.mode = order_mode;
  opts.file_hash_algorithm = file_hash_algo;
  opts.inode.with_similarity = order_mode == file_order_mode::SIMILARITY;
  opts.inode.with_nilsimsa = order_mode == file_order_mode::NILSIMSA;

  auto input = std::make_shared<test::os_access_mock>();
  constexpr int dim = 14;

  input->add_dir("");

  for (int x = 0; x < dim; ++x) {
    input->add_dir(fmt::format("{}", x));
    for (int y = 0; y < dim; ++y) {
      input->add_dir(fmt::format("{}/{}", x, y));
      for (int z = 0; z < dim; ++z) {
        input->add_file(fmt::format("{}/{}/{}", x, y, z),
                        (x + 1) * (y + 1) * (z + 1));
      }
    }
  }

  auto ref = build_dwarfs(lgr, input, "null", bmcfg, opts);

  for (int i = 0; i < 50; ++i) {
    EXPECT_EQ(ref, build_dwarfs(lgr, input, "null", bmcfg, opts));
  }
}

INSTANTIATE_TEST_SUITE_P(
    dwarfs, file_scanner,
    ::testing::Combine(::testing::Values(file_order_mode::PATH,
                                         file_order_mode::SIMILARITY),
                       ::testing::Values(std::nullopt, "xxh3-128")));

class filter : public testing::TestWithParam<dwarfs::test::filter_test_data> {};

TEST_P(filter, filesystem) {
  auto spec = GetParam();

  block_manager::config cfg;
  scanner_options options;

  options.remove_empty_dirs = true;

  std::ostringstream logss;
  stream_logger lgr(logss); // TODO: mock
  lgr.set_policy<prod_logger_policy>();

  auto scr = std::make_shared<builtin_script>(lgr);

  scr->set_root_path("");
  {
    std::istringstream iss(spec.filter());
    scr->add_filter_rules(iss);
  }

  auto input = std::make_shared<test::os_access_mock>();

  for (auto const& [stat, name] : dwarfs::test::test_dirtree()) {
    struct ::stat st;

    std::memset(&st, 0, sizeof(st));

    st.st_ino = stat.st_ino;
    st.st_mode = stat.st_mode;
    st.st_nlink = stat.st_nlink;
    st.st_uid = stat.st_uid;
    st.st_gid = stat.st_gid;
    st.st_size = stat.st_size;
    st.st_atime = stat.atime;
    st.st_mtime = stat.mtime;
    st.st_ctime = stat.ctime;
    st.st_rdev = stat.st_rdev;

    auto path = name.substr(name.size() == 5 ? 5 : 6);

    if (S_ISREG(st.st_mode)) {
      input->add(path, st,
                 [size = st.st_size] { return test::loremipsum(size); });
    } else if (S_ISLNK(st.st_mode)) {
      input->add(path, st, test::loremipsum(st.st_size));
    } else {
      input->add(path, st);
    }
  }

  auto fsimage = build_dwarfs(lgr, input, "null", cfg, options, nullptr, scr);

  auto mm = std::make_shared<test::mmap_mock>(std::move(fsimage));

  filesystem_options opts;
  opts.block_cache.max_bytes = 1 << 20;
  opts.metadata.enable_nlink = true;
  opts.metadata.check_consistency = true;

  filesystem_v2 fs(lgr, mm, opts);

  std::unordered_set<std::string> got;

  fs.walk([&got](dir_entry_view e) { got.emplace(e.path()); });

  EXPECT_EQ(spec.expected_files(), got);
}

INSTANTIATE_TEST_SUITE_P(dwarfs, filter,
                         ::testing::ValuesIn(dwarfs::test::get_filter_tests()));

TEST(file_scanner, input_list) {
  std::ostringstream logss;
  stream_logger lgr(logss); // TODO: mock
  lgr.set_policy<prod_logger_policy>();

  auto bmcfg = block_manager::config();
  auto opts = scanner_options();

  opts.file_order.mode = file_order_mode::NONE;

  auto input = test::os_access_mock::create_test_instance();

  std::vector<std::filesystem::path> input_list{
      "somedir/ipsum.py",
      "foo.pl",
  };

  auto fsimage = build_dwarfs(lgr, input, "null", bmcfg, opts, nullptr, nullptr,
                              input_list);

  auto mm = std::make_shared<test::mmap_mock>(std::move(fsimage));

  filesystem_v2 fs(lgr, mm);

  std::unordered_set<std::string> got;

  fs.walk([&got](dir_entry_view e) { got.emplace(e.path()); });

  std::unordered_set<std::string> expected{
      "",
      "somedir",
      "somedir/ipsum.py",
      "foo.pl",
  };

  EXPECT_EQ(expected, got);
}
