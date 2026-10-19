#include <ReleaseJsonParser.h>
#include <StreamingJsonParser.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr char DIGEST_HEX[] = "8745d7dc37c60052691a2ef124663e43ffe60a28bf51500615634058b1d1ab1f";

std::string asset(const std::string& name, const std::string& digestField) {
  return R"({"name":")" + name + R"(","size":6133040,)" + digestField +
         R"("browser_download_url":"https://github.com/o/r/releases/download/2.37/)" + name + R"("})";
}

void feedAll(ReleaseJsonParser& parser, const std::string& json) {
  // A byte at a time, as a stream may split it anywhere.
  for (char c : json) parser.feed(&c, 1);
}

struct StreamingRecorder {
  std::string strings;
  std::string overflowPrefix;
  std::string parts;
  std::string streamed;
  size_t stringCalls = 0;
  size_t overflowCalls = 0;
  size_t partCalls = 0;
  size_t firstPartCalls = 0;
  size_t lastPartCalls = 0;
  size_t firstPartLength = 0;
  size_t lastPartLength = 0;
  size_t startCalls = 0;
  size_t chunkCalls = 0;
  size_t endCalls = 0;
  bool rejectStart = false;
  bool rejectChunk = false;
  bool rejectEnd = false;

  static StreamingRecorder& self(void* ctx) { return *static_cast<StreamingRecorder*>(ctx); }

  static void onString(void* ctx, const char* value, size_t length) {
    auto& recorder = self(ctx);
    recorder.strings.append(value, length);
    ++recorder.stringCalls;
  }
  static void onOverflow(void* ctx, const char* value, size_t length) {
    auto& recorder = self(ctx);
    recorder.overflowPrefix.append(value, length);
    ++recorder.overflowCalls;
  }
  static void onPart(void* ctx, const char* value, size_t length, bool first, bool last) {
    auto& recorder = self(ctx);
    recorder.parts.append(value, length);
    ++recorder.partCalls;
    if (first) {
      ++recorder.firstPartCalls;
      recorder.firstPartLength = length;
    }
    if (last) {
      ++recorder.lastPartCalls;
      recorder.lastPartLength = length;
    }
  }
  static bool onStart(void* ctx) {
    auto& recorder = self(ctx);
    ++recorder.startCalls;
    return !recorder.rejectStart;
  }
  static bool onChunk(void* ctx, const char* value, size_t length) {
    auto& recorder = self(ctx);
    recorder.streamed.append(value, length);
    ++recorder.chunkCalls;
    return !recorder.rejectChunk;
  }
  static bool onEnd(void* ctx) {
    auto& recorder = self(ctx);
    ++recorder.endCalls;
    return !recorder.rejectEnd;
  }

  JsonCallbacks callbacks() {
    JsonCallbacks cb{};
    cb.ctx = this;
    cb.onString = onString;
    return cb;
  }
};

void feedChunks(StreamingJsonParser& parser, const std::string& json, const size_t chunkSize) {
  for (size_t offset = 0; offset < json.size(); offset += chunkSize) {
    parser.feed(json.data() + offset, std::min(chunkSize, json.size() - offset));
  }
}

bool sameAsDigestHex(const uint8_t* bytes) {
  for (size_t i = 0; i < 32; ++i) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02x", bytes[i]);
    if (memcmp(hex, DIGEST_HEX + 2 * i, 2) != 0) return false;
  }
  return true;
}

TEST(ReleaseJsonParserDigest, ReadsTheFirmwareAssetsSha256) {
  ReleaseJsonParser parser;
  const std::string digest = std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)";
  feedAll(parser, R"({"tag_name":"2.37","assets":[)" +
                      asset("firmware-x4pro.bin", R"("digest":"sha256:)" + std::string(64, 'a') + R"(",)") + "," +
                      asset("firmware.bin", digest) + "]}");
  ASSERT_TRUE(parser.foundFirmware());
  EXPECT_EQ(parser.getFirmwareSize(), 6133040u);
  ASSERT_TRUE(parser.hasFirmwareSha256());
  EXPECT_TRUE(sameAsDigestHex(parser.getFirmwareSha256()));
}

TEST(ReleaseJsonParserDigest, AnotherAssetsDigestIsNotTaken) {
  ReleaseJsonParser parser;
  feedAll(parser, R"({"tag_name":"2.37","assets":[)" +
                      asset("firmware-x4pro.bin", std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)") + "," +
                      asset("firmware.bin", "") + "]}");
  ASSERT_TRUE(parser.foundFirmware());
  EXPECT_FALSE(parser.hasFirmwareSha256());
}

TEST(ReleaseJsonParserDigest, ANullOrMalformedDigestIsNone) {
  for (const std::string& field : {std::string(R"("digest":null,)"), std::string(R"("digest":"sha512:abcd",)"),
                                   std::string(R"("digest":"sha256:)") + std::string(63, 'a') + R"(",)",
                                   std::string(R"("digest":"sha256:)") + std::string(63, 'a') + R"(g",)"}) {
    ReleaseJsonParser parser;
    feedAll(parser, R"({"tag_name":"2.37","assets":[)" + asset("firmware.bin", field) + "]}");
    ASSERT_TRUE(parser.foundFirmware()) << field;
    EXPECT_FALSE(parser.hasFirmwareSha256()) << field;
  }
}

TEST(ReleaseJsonParserDigest, TheBetaListFormCarriesTheDigestToo) {
  ReleaseJsonParser parser;
  feedAll(parser, R"([{"tag_name":"2.38.0-rc.1","assets":[)" +
                      asset("firmware.bin", std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)") + "]}]");
  ASSERT_TRUE(parser.foundFirmware());
  EXPECT_STREQ(parser.getTagName(), "2.38.0-rc.1");
  ASSERT_TRUE(parser.hasFirmwareSha256());
  EXPECT_TRUE(sameAsDigestHex(parser.getFirmwareSha256()));
}

TEST(ReleaseJsonParserDigest, ResetForgetsTheDigest) {
  ReleaseJsonParser parser;
  feedAll(parser, R"({"tag_name":"2.37","assets":[)" +
                      asset("firmware.bin", std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)") + "]}");
  ASSERT_TRUE(parser.hasFirmwareSha256());
  parser.reset();
  EXPECT_FALSE(parser.hasFirmwareSha256());
}

std::string jsonString(const std::string& text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out + "\"";
}

// GitHub's key order: the title before the assets, the notes after them.
std::string release(const std::string& nameField, const std::string& bodyField) {
  return R"({"tag_name":"2.37",)" + nameField + R"("assets":[)" + asset("firmware.bin", "") +
         R"(],"tarball_url":"https://api.github.com/repos/o/r/tarball/2.37")" + bodyField + "}";
}

TEST(ReleaseJsonParserNotes, ReadsTheReleaseTitle) {
  ReleaseJsonParser parser;
  feedAll(parser, release(R"("name":"2.37 Fixes and improvements",)", ""));
  EXPECT_STREQ(parser.getReleaseName(), "2.37 Fixes and improvements");
}

TEST(ReleaseJsonParserNotes, AnAssetsNameIsNotTheTitle) {
  ReleaseJsonParser parser;
  feedAll(parser, release("", ""));
  ASSERT_TRUE(parser.foundFirmware());
  EXPECT_STREQ(parser.getReleaseName(), "");
}

TEST(ReleaseJsonParserNotes, ALongTitleIsCutOnACodepointBoundary) {
  ReleaseJsonParser parser;
  // 62 ASCII bytes, then a two-byte e-acute that would straddle the 63-byte limit.
  feedAll(parser, release(R"("name":")" + std::string(62, 'a') + "\xC3\xA9 more\",", ""));
  EXPECT_EQ(std::string(parser.getReleaseName()), std::string(62, 'a'));
}

TEST(ReleaseJsonParserNotes, ReadsTheNotesOfAShortBody) {
  ReleaseJsonParser parser;
  feedAll(parser, release("", ",\"body\":" + jsonString("Fixes.\r\n\r\n- One\r\n- Two")));
  EXPECT_TRUE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseNotes(), "Fixes.\n- One\n- Two");
}

TEST(ReleaseJsonParserNotes, ALongBodyIsReadFromItsStartBeforeItEnds) {
  std::string body =
      "2.37 improves reading position recovery, chapter page counting, reading statistics, keyboard layouts, and "
      "several navigation issues.\r\n\r\n## Highlights\r\n\r\n- Reading position is now restored correctly after "
      "layout changes.\r\n";
  while (body.size() < 4000) body += "- Fixed font spacing and screen-refresh artefacts.\r\n";
  const std::string json = release("", ",\"body\":" + jsonString(body));

  // Up to well into the notes, but not to their end: the update check stops reading here.
  ReleaseJsonParser parser;
  const size_t cut = json.find("\"body\"") + 1200;
  feedAll(parser, json.substr(0, cut));
  ASSERT_TRUE(parser.foundNotes());
  const std::string notes = parser.getReleaseNotes();
  EXPECT_EQ(notes.rfind(
                "2.37 improves reading position recovery, chapter page counting, reading statistics, keyboard layouts, "
                "and several navigation issues.\n- Reading position is now restored correctly after layout changes.\n",
                0),
            0u)
      << notes;

  // The rest still parses, and the excerpt stands.
  feedAll(parser, json.substr(cut));
  EXPECT_TRUE(parser.foundFirmware());
  EXPECT_EQ(std::string(parser.getReleaseNotes()), notes);
}

TEST(ReleaseJsonParserNotes, ANullBodyCountsAsRead) {
  ReleaseJsonParser parser;
  feedAll(parser, release("", R"(,"body":null)"));
  EXPECT_TRUE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseNotes(), "");
}

TEST(ReleaseJsonParserNotes, NoBodyMeansNoNotes) {
  ReleaseJsonParser parser;
  feedAll(parser, release("", ""));
  EXPECT_FALSE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseNotes(), "");
}

TEST(ReleaseJsonParserNotes, ABodyInsideANestedObjectIsNotTheNotes) {
  ReleaseJsonParser parser;
  feedAll(parser, release(R"("author":{"name":"someone","body":"not the notes"},)", ""));
  EXPECT_FALSE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseName(), "");
}

TEST(ReleaseJsonParserNotes, TheBetaListFormCarriesTitleAndNotes) {
  ReleaseJsonParser parser;
  feedAll(parser, "[" + release(R"("name":"Beta",)", R"(,"body":"Try it.\n")") + "]");
  EXPECT_STREQ(parser.getReleaseName(), "Beta");
  EXPECT_TRUE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseNotes(), "Try it.");
}

TEST(ReleaseJsonParserNotes, ResetForgetsTitleAndNotes) {
  ReleaseJsonParser parser;
  feedAll(parser, release(R"("name":"Beta",)", R"(,"body":"Try it.")"));
  ASSERT_TRUE(parser.foundNotes());
  parser.reset();
  EXPECT_STREQ(parser.getReleaseName(), "");
  EXPECT_FALSE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseNotes(), "");
}


TEST(StreamingJsonParserCallbacks, OverflowPrefixIsDeliveredOnceAndLongValueIsDropped) {
  StreamingRecorder recorder;
  JsonCallbacks callbacks = recorder.callbacks();
  callbacks.onStringOverflow = StreamingRecorder::onOverflow;
  StreamingJsonParser parser(callbacks);
  const std::string body(1200, 'x');
  const std::string json = R"({"body":")" + body + R"(","next":"ok"})";

  feedChunks(parser, json, 17);

  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(recorder.overflowCalls, 1u);
  EXPECT_EQ(recorder.overflowPrefix, body.substr(0, StreamingJsonParser::TOKEN_BUF_SIZE - 1));
  EXPECT_EQ(recorder.stringCalls, 1u);
  EXPECT_EQ(recorder.strings, "ok");
}

TEST(StreamingJsonParserCallbacks, LongStringPartsReassembleWithFirstAndLastMarkers) {
  StreamingRecorder recorder;
  JsonCallbacks callbacks = recorder.callbacks();
  callbacks.onStringPart = StreamingRecorder::onPart;
  StreamingJsonParser parser(callbacks);
  const std::string body(1400, 'p');
  const std::string json = R"({"body":")" + body + R"("})";

  feedChunks(parser, json, 23);

  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(recorder.parts, body);
  EXPECT_GT(recorder.partCalls, 1u);
  EXPECT_EQ(recorder.firstPartCalls, 1u);
  EXPECT_EQ(recorder.lastPartCalls, 1u);
  EXPECT_EQ(recorder.firstPartLength, StreamingJsonParser::TOKEN_BUF_SIZE - 1);
  EXPECT_EQ(recorder.lastPartLength, body.size() - 2 * (StreamingJsonParser::TOKEN_BUF_SIZE - 1));
}

TEST(StreamingJsonParserCallbacks, ResetClearsLongStringPartState) {
  StreamingRecorder recorder;
  JsonCallbacks callbacks = recorder.callbacks();
  callbacks.onStringPart = StreamingRecorder::onPart;
  StreamingJsonParser parser(callbacks);

  const std::string incomplete = R"({"body":")" + std::string(StreamingJsonParser::TOKEN_BUF_SIZE, 'a');
  parser.feed(incomplete.data(), incomplete.size());
  ASSERT_EQ(recorder.firstPartCalls, 1u);

  parser.reset();
  const std::string body(1400, 'b');
  const std::string json = R"({"body":")" + body + R"("})";
  feedChunks(parser, json, 23);

  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(recorder.firstPartCalls, 2u);
  EXPECT_EQ(recorder.parts.substr(StreamingJsonParser::TOKEN_BUF_SIZE - 1), body);
}



TEST(StreamingJsonParserCallbacks, StreamingKeepsFullUnicodeStringAcrossChunks) {
  StreamingRecorder recorder;
  JsonCallbacks callbacks = recorder.callbacks();
  callbacks.onStringStart = StreamingRecorder::onStart;
  callbacks.onStringChunk = StreamingRecorder::onChunk;
  callbacks.onStringEnd = StreamingRecorder::onEnd;
  StreamingJsonParser parser(callbacks);
  const std::string json = R"({"body":")" + std::string(63, 'a') + R"(\uD83D\uDE80)" + std::string(700, 'b') +
                           R"("})";
  const std::string expected = std::string(63, 'a') + "\xF0\x9F\x9A\x80" + std::string(700, 'b');

  feedChunks(parser, json, 11);

  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(recorder.streamed, expected);
  EXPECT_GT(recorder.chunkCalls, 1u);
  EXPECT_EQ(recorder.startCalls, 1u);
  EXPECT_EQ(recorder.endCalls, 1u);
  EXPECT_EQ(recorder.stringCalls, 0u);
}

TEST(StreamingJsonParserCallbacks, StartCallbackCanRejectAString) {
  StreamingRecorder recorder;
  recorder.rejectStart = true;
  JsonCallbacks callbacks = recorder.callbacks();
  callbacks.onStringStart = StreamingRecorder::onStart;
  callbacks.onStringChunk = StreamingRecorder::onChunk;
  callbacks.onStringEnd = StreamingRecorder::onEnd;
  StreamingJsonParser parser(callbacks);
  const std::string json = R"({"body":"value"})";

  feedChunks(parser, json, json.size());

  EXPECT_TRUE(parser.hasError());
  EXPECT_EQ(recorder.startCalls, 1u);
  EXPECT_EQ(recorder.chunkCalls, 0u);
  EXPECT_EQ(recorder.endCalls, 0u);
}

TEST(StreamingJsonParserCallbacks, ChunkCallbackCanRejectAString) {
  StreamingRecorder recorder;
  recorder.rejectChunk = true;
  JsonCallbacks callbacks = recorder.callbacks();
  callbacks.onStringStart = StreamingRecorder::onStart;
  callbacks.onStringChunk = StreamingRecorder::onChunk;
  callbacks.onStringEnd = StreamingRecorder::onEnd;
  StreamingJsonParser parser(callbacks);
  const std::string json = R"({"body":")" + std::string(100, 'v') + R"("})";

  feedChunks(parser, json, json.size());

  EXPECT_TRUE(parser.hasError());
  EXPECT_EQ(recorder.startCalls, 1u);
  EXPECT_EQ(recorder.chunkCalls, 1u);
  EXPECT_EQ(recorder.endCalls, 0u);
}

TEST(StreamingJsonParserCallbacks, EndCallbackCanRejectAString) {
  StreamingRecorder recorder;
  recorder.rejectEnd = true;
  JsonCallbacks callbacks = recorder.callbacks();
  callbacks.onStringStart = StreamingRecorder::onStart;
  callbacks.onStringChunk = StreamingRecorder::onChunk;
  callbacks.onStringEnd = StreamingRecorder::onEnd;
  StreamingJsonParser parser(callbacks);
  const std::string json = R"({"body":"short"})";

  feedChunks(parser, json, json.size());

  EXPECT_TRUE(parser.hasError());
  EXPECT_EQ(recorder.startCalls, 1u);
  EXPECT_EQ(recorder.chunkCalls, 1u);
  EXPECT_EQ(recorder.endCalls, 1u);
}

TEST(StreamingJsonParserCallbacks, OrdinaryTokenOverflowDropsOnlyTheOverlongValue) {
  StreamingRecorder recorder;
  JsonCallbacks callbacks = recorder.callbacks();
  StreamingJsonParser parser(callbacks);
  const std::string body(1200, 'z');
  const std::string json = R"({"body":")" + body + R"(","next":"ok"})";

  feedChunks(parser, json, 31);

  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(recorder.stringCalls, 1u);
  EXPECT_EQ(recorder.strings, "ok");
}

}  // namespace
