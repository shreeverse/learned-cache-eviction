#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace lcache::test {

// A file in GoogleTest's temp dir, named after the running test, deleted when it goes out of scope.
class TempFile {
 public:
  explicit TempFile(std::string_view suffix) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = std::filesystem::path(::testing::TempDir()) /
            (std::string(info->test_suite_name()) + "_" + info->name() + std::string(suffix));
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) = delete;
  TempFile& operator=(TempFile&&) = delete;
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

  void write(std::string_view bytes) const {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

 private:
  std::filesystem::path path_;
};

}  // namespace lcache::test
