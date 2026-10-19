#include <HttpDownloader.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
class FileDownload : public ::testing::TestWithParam<bool> {
 protected:
  std::filesystem::path dir;
  std::string destination;
  HttpDownloader::Session session;
  const HttpDownloader::HeaderList headers{{"x-auth-user", "reader"}, {"x-auth-key", "secret"}};

  void SetUp() override {
    FakeNet::reset();
    downloaderLargestBlock = 16372;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 1024; ++attempt) {
      dir = std::filesystem::temp_directory_path() /
            ("witchhunt-download-" + std::to_string(stamp) + "-" + std::to_string(attempt));
      if (std::filesystem::create_directory(dir)) break;
      dir.clear();
    }
    ASSERT_FALSE(dir.empty());
    destination = (dir / "book.epub").string();
    std::ofstream(destination, std::ios::binary) << "previous validated book";
  }
  void TearDown() override { std::filesystem::remove_all(dir); }
  std::string contents() const {
    std::ifstream file(destination, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
  void serve(std::string body) {
    FakeNet::resource() = std::move(body);
    FakeNet::serving() = true;
  }
  HttpDownloader::DownloadError download(HttpDownloader::ProgressCallback progress = nullptr) {
    return GetParam() ? HttpDownloader::downloadToFile(session, "https://books.example/book", destination, headers,
                                                      std::move(progress))
                      : HttpDownloader::downloadToFile("https://books.example/book", destination, headers,
                                                       std::move(progress));
  }
};

TEST_P(FileDownload, HeaderAuthenticatedBodyReplacesDestinationOnlyAfterCompletion) {
  serve("new complete book");
  EXPECT_EQ(download([this](unsigned int, unsigned int) {
    EXPECT_EQ(contents(), "previous validated book");
    return true;
  }), HttpDownloader::OK);
  EXPECT_EQ(contents(), "new complete book");
  EXPECT_FALSE(std::filesystem::exists(destination + ".part"));
  ASSERT_FALSE(FakeNet::requests().empty());
  EXPECT_NE(FakeNet::requests().front().find("x-auth-key: secret\r\n"), std::string::npos);
}

TEST_P(FileDownload, CancellationPreservesValidatedDestinationAndRemovesPartial) {
  serve(std::string(20000, 'n'));
  EXPECT_EQ(download([](unsigned int, unsigned int) { return false; }), HttpDownloader::ABORTED);
  EXPECT_EQ(contents(), "previous validated book");
  EXPECT_FALSE(std::filesystem::exists(destination + ".part"));
}

TEST_P(FileDownload, HttpErrorPreservesValidatedDestination) {
  FakeNet::replies().push_back({"HTTP/1.1 403 Forbidden\r\nContent-Length: 6\r\n\r\ndenied"});
  EXPECT_EQ(download(), HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(contents(), "previous validated book");
  EXPECT_FALSE(std::filesystem::exists(destination + ".part"));
}

TEST_P(FileDownload, IgnoredRangeRestartsStagedFileWithoutDuplicatingPrefix) {
  std::string body;
  for (size_t i = 0; i < 16000; ++i) body.push_back(static_cast<char>('a' + i % 23));
  serve(body);
  FakeNet::ignoreRangeFrom() = 1;
  EXPECT_EQ(download(), HttpDownloader::OK);
  EXPECT_EQ(contents(), body);
  ASSERT_GE(FakeNet::requests().size(), 2u);
}

TEST_P(FileDownload, InterruptedPublicationRestoresBackupBeforeFailedTransfer) {
  ASSERT_TRUE(std::filesystem::exists(destination));
  std::filesystem::rename(destination, destination + ".bak");
  FakeNet::replies().push_back({"HTTP/1.1 500 Error\r\nContent-Length: 0\r\n\r\n"});
  EXPECT_EQ(download(), HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(contents(), "previous validated book");
}

TEST_F(FileDownload, ReusedSessionClearsBasicAuthAndPreviousExtraHeaders) {
  serve("first book");
  ASSERT_EQ(HttpDownloader::downloadToFile(session, "https://books.example/first", destination, nullptr,
                                            "basic-user", "basic-password"), HttpDownloader::OK);
  const size_t second = FakeNet::requests().size();
  serve("second book");
  ASSERT_EQ(HttpDownloader::downloadToFile(session, "https://books.example/second", destination, headers),
            HttpDownloader::OK);
  ASSERT_GT(FakeNet::requests().size(), second);
  EXPECT_EQ(FakeNet::requests()[second].find("Authorization:"), std::string::npos);
  EXPECT_NE(FakeNet::requests()[second].find("x-auth-key: secret"), std::string::npos);
  const size_t third = FakeNet::requests().size();
  serve("third book");
  ASSERT_EQ(HttpDownloader::downloadToFile(session, "https://books.example/third", destination), HttpDownloader::OK);
  EXPECT_EQ(FakeNet::requests().at(third).find("x-auth-key:"), std::string::npos);
  EXPECT_EQ(FakeNet::requests().at(third).find("Authorization:"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(OneShotAndSession, FileDownload, ::testing::Bool());
}  // namespace
