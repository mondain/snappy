// Copyright 2005 and onwards Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//      Unless required by applicable law or agreed to in writing, software
//      distributed under the License is distributed on an "AS IS" BASIS,
//      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//      See the License for the specific language governing permissions and
//      limitations under the License.

#include <math.h>
#include <stdlib.h>
#include <sys/mman.h>

#include <algorithm>
#include <string>
#include <vector>

#include "snappy.h"
#include "snappy-internal.h"
#include "snappy-test.h"
#include "snappy-sinksource.h"

DEFINE_int32(start_len, -1,
             "Starting prefix size for testing (-1: just full file contents)");
DEFINE_int32(end_len, -1,
             "Starting prefix size for testing (-1: just full file contents)");
DEFINE_int32(bytes, 10485760,
             "How many bytes to compress/uncompress per file for timing");

DEFINE_bool(zlib, false,
            "Run zlib compression (http://www.zlib.net)");
DEFINE_bool(lzo, false,
            "Run LZO compression (http://www.oberhumer.com/opensource/lzo/)");
DEFINE_bool(quicklz, false,
            "Run quickLZ compression (http://www.quicklz.com/)");
DEFINE_bool(liblzf, false,
            "Run libLZF compression "
            "(http://www.goof.com/pcg/marc/liblzf.html)");
DEFINE_bool(fastlz, false,
            "Run FastLZ compression (http://www.fastlz.org/");
DEFINE_bool(snappy, true, "Run snappy compression");


DEFINE_bool(write_compressed, false,
            "Write compressed versions of each file to <file>.comp");
DEFINE_bool(write_uncompressed, false,
            "Write uncompressed versions of each file to <file>.uncomp");

namespace snappy {


// To test against code that reads beyond its input, this class copies a
// string to a newly allocated group of pages, the last of which
// is made unreadable via mprotect. Note that we need to allocate the
// memory with mmap(), as POSIX allows mprotect() only on memory allocated
// with mmap(), and some malloc/posix_memalign implementations expect to
// be able to read previously allocated memory while doing heap allocations.
//
// TODO(user): Add support for running the unittest without the protection
// checks if the target system doesn't have mmap.
class DataEndingAtUnreadablePage {
 public:
  explicit DataEndingAtUnreadablePage(const string& s) {
    const size_t page_size = getpagesize();
    const size_t size = s.size();
    // Round up space for string to a multiple of page_size.
    size_t space_for_string = (size + page_size - 1) & ~(page_size - 1);
    alloc_size_ = space_for_string + page_size;
    mem_ = mmap(NULL, alloc_size_,
                PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    CHECK_NE(MAP_FAILED, mem_);
    protected_page_ = reinterpret_cast<char*>(mem_) + space_for_string;
    char* dst = protected_page_ - size;
    memcpy(dst, s.data(), size);
    data_ = dst;
    size_ = size;
    // Make guard page unreadable.
    CHECK_EQ(0, mprotect(protected_page_, page_size, PROT_NONE));
  }

  ~DataEndingAtUnreadablePage() {
    // Undo the mprotect.
    CHECK_EQ(0, mprotect(protected_page_, getpagesize(), PROT_READ|PROT_WRITE));
    CHECK_EQ(0, munmap(mem_, alloc_size_));
  }

  const char* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  size_t alloc_size_;
  void* mem_;
  char* protected_page_;
  const char* data_;
  size_t size_;
};

enum CompressorType {
  ZLIB, LZO, LIBLZF, QUICKLZ, FASTLZ, SNAPPY,
};

const char* names[] = {
  "ZLIB", "LZO", "LIBLZF", "QUICKLZ", "FASTLZ", "SNAPPY",
};

// Returns true if we successfully compressed, false otherwise
static bool Compress(const char* input, size_t input_size, CompressorType comp,
                     string* compressed) {
  switch (comp) {
#ifdef ZLIB_VERSION
    case ZLIB: {
      compressed->resize(ZLib::MinCompressbufSize(input_size));
      ZLib zlib;
      uLongf destlen = compressed->size();
      int ret = zlib.Compress(
          reinterpret_cast<Bytef*>(string_as_array(compressed)),
          &destlen,
          reinterpret_cast<const Bytef*>(input),
          input_size);
      CHECK_EQ(Z_OK, ret);
      compressed->resize(destlen);
      return true;
    }
#endif  // ZLIB_VERSION

#ifdef LZO_VERSION
    case LZO: {
      compressed->resize(input_size + input_size/64 + 16 + 3);
      unsigned char* mem = new unsigned char[LZO1X_1_15_MEM_COMPRESS];
      lzo_uint destlen;
      int ret = lzo1x_1_15_compress(
          reinterpret_cast<const uint8*>(input),
          input_size,
          reinterpret_cast<uint8*>(string_as_array(compressed)),
          &destlen,
          mem);
      CHECK_EQ(LZO_E_OK, ret);
      delete[] mem;
      compressed->resize(destlen);
      break;
    }
#endif  // LZO_VERSION

#ifdef LZF_VERSION
    case LIBLZF: {
      compressed->resize(input_size - 1);
      int destlen = lzf_compress(input,
                                 input_size,
                                 string_as_array(compressed),
                                 input_size - 1);
      if (destlen == 0) {
        // lzf *can* cause lots of blowup when compressing, so they
        // recommend to limit outsize to insize, and just not compress
        // if it's bigger.  Ideally, we'd just swap input and output.
        compressed->assign(input, input_size);
        destlen = input_size;
      }
      compressed->resize(destlen);
      break;
    }
#endif  // LZF_VERSION

#ifdef QLZ_VERSION_MAJOR
    case QUICKLZ: {
      compressed->resize(input_size + 36000);  // 36000 is used for scratch
      qlz_state_compress *state_compress = new qlz_state_compress;
      int destlen = qlz_compress(input,
                                 string_as_array(compressed),
                                 input_size,
                                 state_compress);
      delete state_compress;
      CHECK_NE(0, destlen);
      compressed->resize(destlen);
      break;
    }
#endif  // QLZ_VERSION_MAJOR

#ifdef FASTLZ_VERSION
    case FASTLZ: {
      int compressed_size = max(static_cast<int>(ceil(input_size * 1.05)),
                                66);
      compressed->resize(compressed_size);
      // Use level 1 compression since we mostly care about speed.
      int destlen = fastlz_compress_level(
          1,
          input,
          input_size,
          string_as_array(compressed));
      compressed->resize(destlen);
      CHECK_NE(destlen, 0);
      break;
    }
#endif  // FASTLZ_VERSION

    case SNAPPY: {
      size_t destlen;
      snappy::RawCompress(input, input_size,
                         string_as_array(compressed),
                         &destlen);
      CHECK_LE(destlen, snappy::MaxCompressedLength(input_size));
      compressed->resize(destlen);
      break;
    }


    default: {
      return false;     // the asked-for library wasn't compiled in
    }
  }
  return true;
}

static bool Uncompress(const string& compressed, CompressorType comp,
                       int size, string* output) {
  switch (comp) {
#ifdef ZLIB_VERSION
    case ZLIB: {
      output->resize(size);
      ZLib zlib;
      uLongf destlen = output->size();
      int ret = zlib.Uncompress(
          reinterpret_cast<Bytef*>(string_as_array(output)),
          &destlen,
          reinterpret_cast<const Bytef*>(compressed.data()),
          compressed.size());
      CHECK_EQ(Z_OK, ret);
      CHECK_EQ(destlen, size);
      break;
    }
#endif  // ZLIB_VERSION

#ifdef LZO_VERSION
    case LZO: {
      output->resize(size);
      lzo_uint destlen;
      int ret = lzo1x_decompress(
          reinterpret_cast<const uint8*>(compressed.data()),
          compressed.size(),
          reinterpret_cast<uint8*>(string_as_array(output)),
          &destlen,
          NULL);
      CHECK_EQ(LZO_E_OK, ret);
      CHECK_EQ(destlen, size);
      break;
    }
#endif  // LZO_VERSION

#ifdef LZF_VERSION
    case LIBLZF: {
      output->resize(size);
      int destlen = lzf_decompress(compressed.data(),
                                   compressed.size(),
                                   string_as_array(output),
                                   output->size());
      if (destlen == 0) {
        // This error probably means we had decided not to compress,
        // and thus have stored input in output directly.
        output->assign(compressed.data(), compressed.size());
        destlen = compressed.size();
      }
      CHECK_EQ(destlen, size);
      break;
    }
#endif  // LZF_VERSION

#ifdef QLZ_VERSION_MAJOR
    case QUICKLZ: {
      output->resize(size);
      qlz_state_decompress *state_decompress = new qlz_state_decompress;
      int destlen = qlz_decompress(compressed.data(),
                                   string_as_array(output),
                                   state_decompress);
      delete state_decompress;
      CHECK_EQ(destlen, size);
      break;
    }
#endif  // QLZ_VERSION_MAJOR

#ifdef FASTLZ_VERSION
    case FASTLZ: {
      output->resize(size);
      int destlen = fastlz_decompress(compressed.data(),
                                      compressed.length(),
                                      string_as_array(output),
                                      size);
      CHECK_EQ(destlen, size);
      break;
    }
#endif  // FASTLZ_VERSION

    case SNAPPY: {
      snappy::RawUncompress(compressed.data(), compressed.size(),
                           string_as_array(output));
      break;
    }


    default: {
      return false;     // the asked-for library wasn't compiled in
    }
  }
  return true;
}

static void Measure(const char* data,
                    size_t length,
                    CompressorType comp,
                    int repeats,
                    int block_size) {
  // Run tests a few time and pick median running times
  static const int kRuns = 5;
  double ctime[kRuns];
  double utime[kRuns];
  int compressed_size = 0;

  {
    // Chop the input into blocks
    int num_blocks = (length + block_size - 1) / block_size;
    vector<const char*> input(num_blocks);
    vector<size_t> input_length(num_blocks);
    vector<string> compressed(num_blocks);
    vector<string> output(num_blocks);
    for (int b = 0; b < num_blocks; b++) {
      int input_start = b * block_size;
      int input_limit = min<int>((b+1)*block_size, length);
      input[b] = data+input_start;
      input_length[b] = input_limit-input_start;

      // Pre-grow the output buffer so we don't measure stupid string
      // append time
      compressed[b].resize(block_size * 2);
    }

    // First, try one trial compression to make sure the code is compiled in
    if (!Compress(input[0], input_length[0], comp, &compressed[0])) {
      LOG(WARNING) << "Skipping " << names[comp] << ": "
                   << "library not compiled in";
      return;
    }

    for (int run = 0; run < kRuns; run++) {
      CycleTimer ctimer, utimer;

      ctimer.Start();
      for (int b = 0; b < num_blocks; b++)
        for (int i = 0; i < repeats; i++)
          Compress(input[b], input_length[b], comp, &compressed[b]);
      ctimer.Stop();

      for (int b = 0; b < num_blocks; b++) {
        output[b].resize(input_length[b]);
      }

      utimer.Start();
      for (int i = 0; i < repeats; i++)
        for (int b = 0; b < num_blocks; b++)
          Uncompress(compressed[b], comp, input_length[b], &output[b]);
      utimer.Stop();

      ctime[run] = ctimer.Get();
      utime[run] = utimer.Get();
    }

    compressed_size = 0;
    for (int i = 0; i < compressed.size(); i++) {
      compressed_size += compressed[i].size();
    }
  }

  sort(ctime, ctime + kRuns);
  sort(utime, utime + kRuns);
  const int med = kRuns/2;

  float comp_rate = (length / ctime[med]) * repeats / 1048576.0;
  float uncomp_rate = (length / utime[med]) * repeats / 1048576.0;
  string x = names[comp];
  x += ":";
  string urate = (uncomp_rate >= 0)
                 ? StringPrintf("%.1f", uncomp_rate)
                 : string("?");
  printf("%-7s [b %dM] bytes %6d -> %6d %4.1f%%  "
         "comp %5.1f MB/s  uncomp %5s MB/s\n",
         x.c_str(),
         block_size/(1<<20),
         static_cast<int>(length), static_cast<uint32>(compressed_size),
         (compressed_size * 100.0) / max<int>(1, length),
         comp_rate,
         urate.c_str());
}


static int VerifyString(const string& input) {
  string compressed;
  DataEndingAtUnreadablePage i(input);
  const size_t written = snappy::Compress(i.data(), i.size(), &compressed);
  CHECK_EQ(written, compressed.size());
  CHECK_LE(compressed.size(),
           snappy::MaxCompressedLength(input.size()));
  CHECK(snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));

  string uncompressed;
  DataEndingAtUnreadablePage c(compressed);
  CHECK(snappy::Uncompress(c.data(), c.size(), &uncompressed));
  CHECK_EQ(uncompressed, input);
  return uncompressed.size();
}


// Test that data compressed by a compressor that does not
// obey block sizes is uncompressed properly.
static void VerifyNonBlockedCompression(const string& input) {
  if (input.length() > snappy::kBlockSize) {
    // We cannot test larger blocks than the maximum block size, obviously.
    return;
  }

  string prefix;
  Varint::Append32(&prefix, input.size());

  // Setup compression table
  snappy::internal::WorkingMemory wmem;
  int table_size;
  uint16* table = wmem.GetHashTable(input.size(), &table_size);

  // Compress entire input in one shot
  string compressed;
  compressed += prefix;
  compressed.resize(prefix.size()+snappy::MaxCompressedLength(input.size()));
  char* dest = string_as_array(&compressed) + prefix.size();
  char* end = snappy::internal::CompressFragment(input.data(), input.size(),
                                                dest, table, table_size);
  compressed.resize(end - compressed.data());

  // Uncompress into string
  string uncomp_str;
  CHECK(snappy::Uncompress(compressed.data(), compressed.size(), &uncomp_str));
  CHECK_EQ(uncomp_str, input);

}

// Expand the input so that it is at least K times as big as block size
static string Expand(const string& input) {
  static const int K = 3;
  string data = input;
  while (data.size() < K * snappy::kBlockSize) {
    data += input;
  }
  return data;
}

static int Verify(const string& input) {
  VLOG(1) << "Verifying input of size " << input.size();

  // Compress using string based routines
  const int result = VerifyString(input);


  VerifyNonBlockedCompression(input);
  if (!input.empty()) {
    VerifyNonBlockedCompression(Expand(input));
  }


  return result;
}

// This test checks to ensure that snappy doesn't coredump if it gets
// corrupted data.

static bool IsValidCompressedBuffer(const string& c) {
  return snappy::IsValidCompressedBuffer(c.data(), c.size());
}
static bool Uncompress(const string& c, string* u) {
  return snappy::Uncompress(c.data(), c.size(), u);
}

TYPED_TEST(CorruptedTest, VerifyCorrupted) {
  string source = "making sure we don't crash with corrupted input";
  VLOG(1) << source;
  string dest;
  TypeParam uncmp;
  snappy::Compress(source.data(), source.size(), &dest);

  // Mess around with the data. It's hard to simulate all possible
  // corruptions; this is just one example ...
  CHECK_GT(dest.size(), 3);
  dest[1]--;
  dest[3]++;
  // this really ought to fail.
  CHECK(!IsValidCompressedBuffer(TypeParam(dest)));
  CHECK(!Uncompress(TypeParam(dest), &uncmp));

  // This is testing for a security bug - a buffer that decompresses to 100k
  // but we lie in the snappy header and only reserve 0 bytes of memory :)
  source.resize(100000);
  for (int i = 0; i < source.length(); ++i) {
    source[i] = 'A';
  }
  snappy::Compress(source.data(), source.size(), &dest);
  dest[0] = dest[1] = dest[2] = dest[3] = 0;
  CHECK(!IsValidCompressedBuffer(TypeParam(dest)));
  CHECK(!Uncompress(TypeParam(dest), &uncmp));

  if (sizeof(void *) == 4) {
    // Another security check; check a crazy big length can't DoS us with an
    // over-allocation.
    // Currently this is done only for 32-bit builds.  On 64-bit builds,
    // where 3GBytes might be an acceptable allocation size, Uncompress()
    // attempts to decompress, and sometimes causes the test to run out of
    // memory.
    dest[0] = dest[1] = dest[2] = dest[3] = 0xff;
    // This decodes to a really large size, i.e., 3221225471 bytes
    dest[4] = 'k';
    CHECK(!IsValidCompressedBuffer(TypeParam(dest)));
    CHECK(!Uncompress(TypeParam(dest), &uncmp));
    dest[0] = dest[1] = dest[2] = 0xff;
    dest[3] = 0x7f;
    CHECK(!IsValidCompressedBuffer(TypeParam(dest)));
    CHECK(!Uncompress(TypeParam(dest), &uncmp));
  } else {
    LOG(WARNING) << "Crazy decompression lengths not checked on 64-bit build";
  }

  // try reading stuff in from a bad file.
  for (int i = 1; i <= 3; ++i) {
    string data = ReadTestDataFile(StringPrintf("baddata%d.snappy", i).c_str());
    string uncmp;
    // check that we don't return a crazy length
    size_t ulen;
    CHECK(!snappy::GetUncompressedLength(data.data(), data.size(), &ulen)
          || (ulen < (1<<20)));
    uint32 ulen2;
    snappy::ByteArraySource source(data.data(), data.size());
    CHECK(!snappy::GetUncompressedLength(&source, &ulen2) ||
          (ulen2 < (1<<20)));
    CHECK(!IsValidCompressedBuffer(TypeParam(data)));
    CHECK(!Uncompress(TypeParam(data), &uncmp));
  }
}

// Helper routines to construct arbitrary compressed strings.
// These mirror the compression code in snappy.cc, but are copied
// here so that we can bypass some limitations in the how snappy.cc
// invokes these routines.
static void AppendLiteral(string* dst, const string& literal) {
  if (literal.empty()) return;
  int n = literal.size() - 1;
  if (n < 60) {
    // Fit length in tag byte
    dst->push_back(0 | (n << 2));
  } else {
    // Encode in upcoming bytes
    char number[4];
    int count = 0;
    while (n > 0) {
      number[count++] = n & 0xff;
      n >>= 8;
    }
    dst->push_back(0 | ((59+count) << 2));
    *dst += string(number, count);
  }
  *dst += literal;
}

static void AppendCopy(string* dst, int offset, int length) {
  while (length > 0) {
    // Figure out how much to copy in one shot
    int to_copy;
    if (length >= 68) {
      to_copy = 64;
    } else if (length > 64) {
      to_copy = 60;
    } else {
      to_copy = length;
    }
    length -= to_copy;

    if ((to_copy < 12) && (offset < 2048)) {
      assert(to_copy-4 < 8);            // Must fit in 3 bits
      dst->push_back(1 | ((to_copy-4) << 2) | ((offset >> 8) << 5));
      dst->push_back(offset & 0xff);
    } else if (offset < 65536) {
      dst->push_back(2 | ((to_copy-1) << 2));
      dst->push_back(offset & 0xff);
      dst->push_back(offset >> 8);
    } else {
      dst->push_back(3 | ((to_copy-1) << 2));
      dst->push_back(offset & 0xff);
      dst->push_back((offset >> 8) & 0xff);
      dst->push_back((offset >> 16) & 0xff);
      dst->push_back((offset >> 24) & 0xff);
    }
  }
}

TEST(Snappy, SimpleTests) {
  Verify("");
  Verify("a");
  Verify("ab");
  Verify("abc");

  Verify("aaaaaaa" + string(16, 'b') + string("aaaaa") + "abc");
  Verify("aaaaaaa" + string(256, 'b') + string("aaaaa") + "abc");
  Verify("aaaaaaa" + string(2047, 'b') + string("aaaaa") + "abc");
  Verify("aaaaaaa" + string(65536, 'b') + string("aaaaa") + "abc");
  Verify("abcaaaaaaa" + string(65536, 'b') + string("aaaaa") + "abc");
}

// Verify max blowup (lots of four-byte copies)
TEST(Snappy, MaxBlowup) {
  string input;
  for (int i = 0; i < 20000; i++) {
    ACMRandom rnd(i);
    uint32 bytes = static_cast<uint32>(rnd.Next());
    input.append(reinterpret_cast<char*>(&bytes), sizeof(bytes));
  }
  for (int i = 19999; i >= 0; i--) {
    ACMRandom rnd(i);
    uint32 bytes = static_cast<uint32>(rnd.Next());
    input.append(reinterpret_cast<char*>(&bytes), sizeof(bytes));
  }
  Verify(input);
}

TEST(Snappy, RandomData) {
  ACMRandom rnd(FLAGS_test_random_seed);

  const int num_ops = 20000;
  for (int i = 0; i < num_ops; i++) {
    if ((i % 1000) == 0) {
      VLOG(0) << "Random op " << i << " of " << num_ops;
    }

    string x;
    int len = rnd.Uniform(4096);
    if (i < 100) {
      len = 65536 + rnd.Uniform(65536);
    }
    while (x.size() < len) {
      int run_len = 1;
      if (rnd.OneIn(10)) {
        run_len = rnd.Skewed(8);
      }
      char c = (i < 100) ? rnd.Uniform(256) : rnd.Skewed(3);
      while (run_len-- > 0 && x.size() < len) {
        x += c;
      }
    }

    Verify(x);
  }
}

TEST(Snappy, FourByteOffset) {
  // The new compressor cannot generate four-byte offsets since
  // it chops up the input into 32KB pieces.  So we hand-emit the
  // copy manually.

  // The two fragments that make up the input string
  string fragment1 = "012345689abcdefghijklmnopqrstuvwxyz";
  string fragment2 = "some other string";

  // How many times is each fragment emittedn
  const int n1 = 2;
  const int n2 = 100000 / fragment2.size();
  const int length = n1 * fragment1.size() + n2 * fragment2.size();

  string compressed;
  Varint::Append32(&compressed, length);

  AppendLiteral(&compressed, fragment1);
  string src = fragment1;
  for (int i = 0; i < n2; i++) {
    AppendLiteral(&compressed, fragment2);
    src += fragment2;
  }
  AppendCopy(&compressed, src.size(), fragment1.size());
  src += fragment1;
  CHECK_EQ(length, src.size());

  string uncompressed;
  CHECK(snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));
  CHECK(snappy::Uncompress(compressed.data(), compressed.size(), &uncompressed));
  CHECK_EQ(uncompressed, src);
}


static bool CheckUncompressedLength(const string& compressed,
                                    size_t* ulength) {
  const bool result1 = snappy::GetUncompressedLength(compressed.data(),
                                                    compressed.size(),
                                                    ulength);

  snappy::ByteArraySource source(compressed.data(), compressed.size());
  uint32 length;
  const bool result2 = snappy::GetUncompressedLength(&source, &length);
  CHECK_EQ(result1, result2);
  return result1;
}

TEST(SnappyCorruption, TruncatedVarint) {
  string compressed, uncompressed;
  size_t ulength;
  compressed.push_back('\xf0');
  CHECK(!CheckUncompressedLength(compressed, &ulength));
  CHECK(!snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));
  CHECK(!snappy::Uncompress(compressed.data(), compressed.size(),
                           &uncompressed));
}

TEST(SnappyCorruption, UnterminatedVarint) {
  string compressed, uncompressed;
  size_t ulength;
  compressed.push_back(128);
  compressed.push_back(128);
  compressed.push_back(128);
  compressed.push_back(128);
  compressed.push_back(128);
  compressed.push_back(10);
  CHECK(!CheckUncompressedLength(compressed, &ulength));
  CHECK(!snappy::IsValidCompressedBuffer(compressed.data(), compressed.size()));
  CHECK(!snappy::Uncompress(compressed.data(), compressed.size(),
                           &uncompressed));
}

TEST(Snappy, ReadPastEndOfBuffer) {
  // Check that we do not read past end of input

  // Make a compressed string that ends with a single-byte literal
  string compressed;
  Varint::Append32(&compressed, 1);
  AppendLiteral(&compressed, "x");

  string uncompressed;
  DataEndingAtUnreadablePage c(compressed);
  CHECK(snappy::Uncompress(c.data(), c.size(), &uncompressed));
  CHECK_EQ(uncompressed, string("x"));
}

// Check for an infinite loop caused by a copy with offset==0
TEST(Snappy, ZeroOffsetCopy) {
  const char* compressed = "\x40\x12\x00\x00";
  //  \x40              Length (must be > kMaxIncrementCopyOverflow)
  //  \x12\x00\x00      Copy with offset==0, length==5
  char uncompressed[100];
  EXPECT_FALSE(snappy::RawUncompress(compressed, 4, uncompressed));
}

TEST(Snappy, ZeroOffsetCopyValidation) {
  const char* compressed = "\x05\x12\x00\x00";
  //  \x05              Length
  //  \x12\x00\x00      Copy with offset==0, length==5
  EXPECT_FALSE(snappy::IsValidCompressedBuffer(compressed, 4));
}


namespace {

int TestFindMatchLength(const char* s1, const char *s2, unsigned length) {
  return snappy::internal::FindMatchLength(s1, s2, s2 + length);
}

}  // namespace

TEST(Snappy, FindMatchLength) {
  // Exercise all different code paths through the function.
  // 64-bit version:

  // Hit s1_limit in 64-bit loop, hit s1_limit in single-character loop.
  EXPECT_EQ(6, TestFindMatchLength("012345", "012345", 6));
  EXPECT_EQ(11, TestFindMatchLength("01234567abc", "01234567abc", 11));

  // Hit s1_limit in 64-bit loop, find a non-match in single-character loop.
  EXPECT_EQ(9, TestFindMatchLength("01234567abc", "01234567axc", 9));

  // Same, but edge cases.
  EXPECT_EQ(11, TestFindMatchLength("01234567abc!", "01234567abc!", 11));
  EXPECT_EQ(11, TestFindMatchLength("01234567abc!", "01234567abc?", 11));

  // Find non-match at once in first loop.
  EXPECT_EQ(0, TestFindMatchLength("01234567xxxxxxxx", "?1234567xxxxxxxx", 16));
  EXPECT_EQ(1, TestFindMatchLength("01234567xxxxxxxx", "0?234567xxxxxxxx", 16));
  EXPECT_EQ(4, TestFindMatchLength("01234567xxxxxxxx", "01237654xxxxxxxx", 16));
  EXPECT_EQ(7, TestFindMatchLength("01234567xxxxxxxx", "0123456?xxxxxxxx", 16));

  // Find non-match in first loop after one block.
  EXPECT_EQ(8, TestFindMatchLength("abcdefgh01234567xxxxxxxx",
                                   "abcdefgh?1234567xxxxxxxx", 24));
  EXPECT_EQ(9, TestFindMatchLength("abcdefgh01234567xxxxxxxx",
                                   "abcdefgh0?234567xxxxxxxx", 24));
  EXPECT_EQ(12, TestFindMatchLength("abcdefgh01234567xxxxxxxx",
                                    "abcdefgh01237654xxxxxxxx", 24));
  EXPECT_EQ(15, TestFindMatchLength("abcdefgh01234567xxxxxxxx",
                                    "abcdefgh0123456?xxxxxxxx", 24));

  // 32-bit version:

  // Short matches.
  EXPECT_EQ(0, TestFindMatchLength("01234567", "?1234567", 8));
  EXPECT_EQ(1, TestFindMatchLength("01234567", "0?234567", 8));
  EXPECT_EQ(2, TestFindMatchLength("01234567", "01?34567", 8));
  EXPECT_EQ(3, TestFindMatchLength("01234567", "012?4567", 8));
  EXPECT_EQ(4, TestFindMatchLength("01234567", "0123?567", 8));
  EXPECT_EQ(5, TestFindMatchLength("01234567", "01234?67", 8));
  EXPECT_EQ(6, TestFindMatchLength("01234567", "012345?7", 8));
  EXPECT_EQ(7, TestFindMatchLength("01234567", "0123456?", 8));
  EXPECT_EQ(7, TestFindMatchLength("01234567", "0123456?", 7));
  EXPECT_EQ(7, TestFindMatchLength("01234567!", "0123456??", 7));

  // Hit s1_limit in 32-bit loop, hit s1_limit in single-character loop.
  EXPECT_EQ(10, TestFindMatchLength("xxxxxxabcd", "xxxxxxabcd", 10));
  EXPECT_EQ(10, TestFindMatchLength("xxxxxxabcd?", "xxxxxxabcd?", 10));
  EXPECT_EQ(13, TestFindMatchLength("xxxxxxabcdef", "xxxxxxabcdef", 13));

  // Same, but edge cases.
  EXPECT_EQ(12, TestFindMatchLength("xxxxxx0123abc!", "xxxxxx0123abc!", 12));
  EXPECT_EQ(12, TestFindMatchLength("xxxxxx0123abc!", "xxxxxx0123abc?", 12));

  // Hit s1_limit in 32-bit loop, find a non-match in single-character loop.
  EXPECT_EQ(11, TestFindMatchLength("xxxxxx0123abc", "xxxxxx0123axc", 13));

  // Find non-match at once in first loop.
  EXPECT_EQ(6, TestFindMatchLength("xxxxxx0123xxxxxxxx",
                                   "xxxxxx?123xxxxxxxx", 18));
  EXPECT_EQ(7, TestFindMatchLength("xxxxxx0123xxxxxxxx",
                                   "xxxxxx0?23xxxxxxxx", 18));
  EXPECT_EQ(8, TestFindMatchLength("xxxxxx0123xxxxxxxx",
                                   "xxxxxx0132xxxxxxxx", 18));
  EXPECT_EQ(9, TestFindMatchLength("xxxxxx0123xxxxxxxx",
                                   "xxxxxx012?xxxxxxxx", 18));

  // Same, but edge cases.
  EXPECT_EQ(6, TestFindMatchLength("xxxxxx0123", "xxxxxx?123", 10));
  EXPECT_EQ(7, TestFindMatchLength("xxxxxx0123", "xxxxxx0?23", 10));
  EXPECT_EQ(8, TestFindMatchLength("xxxxxx0123", "xxxxxx0132", 10));
  EXPECT_EQ(9, TestFindMatchLength("xxxxxx0123", "xxxxxx012?", 10));

  // Find non-match in first loop after one block.
  EXPECT_EQ(10, TestFindMatchLength("xxxxxxabcd0123xx",
                                    "xxxxxxabcd?123xx", 16));
  EXPECT_EQ(11, TestFindMatchLength("xxxxxxabcd0123xx",
                                    "xxxxxxabcd0?23xx", 16));
  EXPECT_EQ(12, TestFindMatchLength("xxxxxxabcd0123xx",
                                    "xxxxxxabcd0132xx", 16));
  EXPECT_EQ(13, TestFindMatchLength("xxxxxxabcd0123xx",
                                    "xxxxxxabcd012?xx", 16));

  // Same, but edge cases.
  EXPECT_EQ(10, TestFindMatchLength("xxxxxxabcd0123", "xxxxxxabcd?123", 14));
  EXPECT_EQ(11, TestFindMatchLength("xxxxxxabcd0123", "xxxxxxabcd0?23", 14));
  EXPECT_EQ(12, TestFindMatchLength("xxxxxxabcd0123", "xxxxxxabcd0132", 14));
  EXPECT_EQ(13, TestFindMatchLength("xxxxxxabcd0123", "xxxxxxabcd012?", 14));
}

TEST(Snappy, FindMatchLengthRandom) {
  const int kNumTrials = 10000;
  const int kTypicalLength = 10;
  ACMRandom rnd(FLAGS_test_random_seed);

  for (int i = 0; i < kNumTrials; i++) {
    string s, t;
    char a = rnd.Rand8();
    char b = rnd.Rand8();
    while (!rnd.OneIn(kTypicalLength)) {
      s.push_back(rnd.OneIn(2) ? a : b);
      t.push_back(rnd.OneIn(2) ? a : b);
    }
    DataEndingAtUnreadablePage u(s);
    DataEndingAtUnreadablePage v(t);
    int matched = snappy::internal::FindMatchLength(
        u.data(), v.data(), v.data() + t.size());
    if (matched == t.size()) {
      EXPECT_EQ(s, t);
    } else {
      EXPECT_NE(s[matched], t[matched]);
      for (int j = 0; j < matched; j++) {
        EXPECT_EQ(s[j], t[j]);
      }
    }
  }
}


static void CompressFile(const char* fname) {
  string fullinput;
  File::ReadFileToStringOrDie(fname, &fullinput);

  string compressed;
  compressed.resize(fullinput.size() * 2);
  Compress(fullinput.data(), fullinput.size(), SNAPPY, &compressed);

  File::WriteStringToFileOrDie(compressed,
                               string(fname).append(".comp").c_str());
}

static void UncompressFile(const char* fname) {
  string fullinput;
  File::ReadFileToStringOrDie(fname, &fullinput);

  size_t uncompLength;
  CHECK(CheckUncompressedLength(fullinput, &uncompLength));

  string uncompressed;
  uncompressed.resize(uncompLength);
  CHECK(snappy::Uncompress(fullinput.data(), fullinput.size(), &uncompressed));

  File::WriteStringToFileOrDie(uncompressed,
                               string(fname).append(".uncomp").c_str());
}

static void MeasureFile(const char* fname) {
  string fullinput;
  File::ReadFileToStringOrDie(fname, &fullinput);
  printf("%-40s :\n", fname);

  int start_len = (FLAGS_start_len < 0) ? fullinput.size() : FLAGS_start_len;
  int end_len = fullinput.size();
  if (FLAGS_end_len >= 0) {
    end_len = min<int>(fullinput.size(), FLAGS_end_len);
  }
  for (int len = start_len; len <= end_len; len++) {
    const char* const input = fullinput.data();
    int repeats = (FLAGS_bytes + len) / (len + 1);
    if (FLAGS_zlib)     Measure(input, len, ZLIB, repeats, 1024<<10);
    if (FLAGS_lzo)      Measure(input, len, LZO, repeats, 1024<<10);
    if (FLAGS_liblzf)   Measure(input, len, LIBLZF, repeats, 1024<<10);
    if (FLAGS_quicklz)  Measure(input, len, QUICKLZ, repeats, 1024<<10);
    if (FLAGS_fastlz)   Measure(input, len, FASTLZ, repeats, 1024<<10);
    if (FLAGS_snappy)    Measure(input, len, SNAPPY, repeats, 4096<<10);

    // For block-size based measurements
    if (0 && FLAGS_snappy) {
      Measure(input, len, SNAPPY, repeats, 8<<10);
      Measure(input, len, SNAPPY, repeats, 16<<10);
      Measure(input, len, SNAPPY, repeats, 32<<10);
      Measure(input, len, SNAPPY, repeats, 64<<10);
      Measure(input, len, SNAPPY, repeats, 256<<10);
      Measure(input, len, SNAPPY, repeats, 1024<<10);
    }
  }
}

static struct {
  const char* label;
  const char* filename;
} files[] = {
  { "html", "html" },
  { "urls", "urls.10K" },
  { "jpg", "house.jpg" },
  { "pdf", "mapreduce-osdi-1.pdf" },
  { "html4", "html_x_4" },
  { "cp", "cp.html" },
  { "c", "fields.c" },
  { "lsp", "grammar.lsp" },
  { "xls", "kennedy.xls" },
  { "txt1", "alice29.txt" },
  { "txt2", "asyoulik.txt" },
  { "txt3", "lcet10.txt" },
  { "txt4", "plrabn12.txt" },
  { "bin", "ptt5" },
  { "sum", "sum" },
  { "man", "xargs.1" },
  { "pb", "geo.protodata" },
  { "gaviota", "kppkn.gtb" },
};

static void BM_UFlat(int iters, int arg) {
  StopBenchmarkTiming();

  // Pick file to process based on "arg"
  CHECK_GE(arg, 0);
  CHECK_LT(arg, ARRAYSIZE(files));
  string contents = ReadTestDataFile(files[arg].filename);

  string zcontents;
  snappy::Compress(contents.data(), contents.size(), &zcontents);
  char* dst = new char[contents.size()];

  SetBenchmarkBytesProcessed(static_cast<int64>(iters) *
                             static_cast<int64>(contents.size()));
  SetBenchmarkLabel(files[arg].label);
  StartBenchmarkTiming();
  while (iters-- > 0) {
    CHECK(snappy::RawUncompress(zcontents.data(), zcontents.size(), dst));
  }
  StopBenchmarkTiming();

  delete[] dst;
}
BENCHMARK(BM_UFlat)->DenseRange(0, 17);

static void BM_UValidate(int iters, int arg) {
  StopBenchmarkTiming();

  // Pick file to process based on "arg"
  CHECK_GE(arg, 0);
  CHECK_LT(arg, ARRAYSIZE(files));
  string contents = ReadTestDataFile(files[arg].filename);

  string zcontents;
  snappy::Compress(contents.data(), contents.size(), &zcontents);

  SetBenchmarkBytesProcessed(static_cast<int64>(iters) *
                             static_cast<int64>(contents.size()));
  SetBenchmarkLabel(files[arg].label);
  StartBenchmarkTiming();
  while (iters-- > 0) {
    CHECK(snappy::IsValidCompressedBuffer(zcontents.data(), zcontents.size()));
  }
  StopBenchmarkTiming();
}
BENCHMARK(BM_UValidate)->DenseRange(0, 4);


static void BM_ZFlat(int iters, int arg) {
  StopBenchmarkTiming();

  // Pick file to process based on "arg"
  CHECK_GE(arg, 0);
  CHECK_LT(arg, ARRAYSIZE(files));
  string contents = ReadTestDataFile(files[arg].filename);

  char* dst = new char[snappy::MaxCompressedLength(contents.size())];

  SetBenchmarkBytesProcessed(static_cast<int64>(iters) *
                             static_cast<int64>(contents.size()));
  StartBenchmarkTiming();

  size_t zsize = 0;
  while (iters-- > 0) {
    snappy::RawCompress(contents.data(), contents.size(), dst, &zsize);
  }
  StopBenchmarkTiming();
  const double compression_ratio =
      static_cast<double>(zsize) / std::max<size_t>(1, contents.size());
  SetBenchmarkLabel(StringPrintf("%s (%.2f %%)",
                                 files[arg].label, 100.0 * compression_ratio));
  VLOG(0) << StringPrintf("compression for %s: %zd -> %zd bytes",
                          files[arg].label, contents.size(), zsize);
  delete[] dst;
}
BENCHMARK(BM_ZFlat)->DenseRange(0, 17);


}  // namespace snappy


int main(int argc, char** argv) {
  InitGoogle(argv[0], &argc, &argv, true);
  File::Init();
  RunSpecifiedBenchmarks();


  if (argc >= 2) {
    for (int arg = 1; arg < argc; arg++) {
      if (FLAGS_write_compressed) {
        CompressFile(argv[arg]);
      } else if (FLAGS_write_uncompressed) {
        UncompressFile(argv[arg]);
      } else {
        MeasureFile(argv[arg]);
      }
    }
    return 0;
  }

  return RUN_ALL_TESTS();
}
