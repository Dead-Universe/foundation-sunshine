#ifdef _WIN32
#include "src/file_clipboard/file_clipboard_wire.h"
#include "src/file_clipboard.h"
#include <gtest/gtest.h>
#include <shlobj.h>
namespace fc = streaming::file_clipboard;
namespace wire = fc::wire;

TEST(FileClipboard, RejectsTraversalDevicesAndAmbiguousNames) {
  for (const auto *name : {L"..\\secret", L"C:\\secret", L"\\server\\file", L"a\\..\\b", L"a/b", L"a:",
                           L"CON.txt", L"a\\LPT1", L"COM\u00b9.txt", L"a.", L"a ", L"a\\", L"a\\\\b"}) {
    EXPECT_FALSE(fc::valid_relative_path(name));
  }
  EXPECT_FALSE(fc::valid_relative_path(std::wstring(1, wchar_t(0xd800))));
  EXPECT_TRUE(fc::valid_relative_path(L"\u4e2d\u6587\\file.txt"));
}
TEST(FileClipboard, ValidatesDirectoryHierarchyAndCaseInsensitiveCollisions) {
  EXPECT_FALSE(wire::valid_manifest({{L"missing\\file", 1, FILE_ATTRIBUTE_NORMAL, {}}}));
  EXPECT_FALSE(wire::valid_manifest({{L"A.txt", 1, FILE_ATTRIBUTE_NORMAL, {}}, {L"a.TXT", 1, FILE_ATTRIBUTE_NORMAL, {}}}));
  EXPECT_FALSE(wire::valid_manifest({{L"folder", 1, FILE_ATTRIBUTE_DIRECTORY, {}}}));
  EXPECT_FALSE(wire::valid_manifest({{L"file", UINT64_MAX, FILE_ATTRIBUTE_NORMAL, {}}}));
  EXPECT_FALSE(wire::valid_manifest({{L"link", 0, FILE_ATTRIBUTE_REPARSE_POINT, {}}}));
}
TEST(FileClipboard, ManifestPreservesUnicodeEmptyDirectoriesAnd64BitSizes) {
  const std::vector<fc::entry> original {{L"\u4e2d\u6587", 0, FILE_ATTRIBUTE_DIRECTORY, {1, 2}},
      {L"\u4e2d\u6587\\empty", 0, FILE_ATTRIBUTE_DIRECTORY, {}},
      {L"\u4e2d\u6587\\large.bin", (std::uint64_t(1) << 32) + 27, FILE_ATTRIBUTE_NORMAL, {12, 34}}};
  auto body = wire::manifest(original); std::vector<fc::entry> decoded;
  ASSERT_TRUE(wire::manifest(body, decoded)); ASSERT_EQ(decoded.size(), original.size());
  EXPECT_EQ(decoded[0].relative_path, original[0].relative_path); EXPECT_EQ(decoded[2].size, original[2].size);
  EXPECT_EQ(decoded[2].last_write.dwHighDateTime, 34u);
  body.pop_back(); EXPECT_FALSE(wire::manifest(body, decoded));
}
TEST(FileClipboard, RejectsTruncatedOversizedAndUnknownFrames) {
  wire::message decoded; auto data = wire::encode({wire::kind::offer, 18, {1, 2, 3}});
  ASSERT_TRUE(wire::decode(data, decoded)); EXPECT_EQ(decoded.id, 18u);
  data.pop_back(); EXPECT_FALSE(wire::decode(data, decoded));
  data = wire::encode({wire::kind::offer, 18, {}}); data[0] = 255;
  EXPECT_FALSE(wire::decode(data, decoded));
  data[0] = 2; data[12] = 255; data[13] = 255; data[14] = 255; data[15] = 127;
  EXPECT_FALSE(wire::decode(data, decoded));
}
TEST(FileClipboard, ReadsCannotEscapeOfferBounds) {
  const std::vector<fc::entry> entries {{L"empty", 0, FILE_ATTRIBUTE_DIRECTORY, {}}, {L"file", 10, FILE_ATTRIBUTE_NORMAL, {}}};
  EXPECT_TRUE(wire::within({17, 1, 8, 2}, 17, entries));
  EXPECT_FALSE(wire::within({16, 1, 8, 2}, 17, entries));
  EXPECT_FALSE(wire::within({17, 0, 0, 0}, 17, entries));
  EXPECT_FALSE(wire::within({17, 1, UINT64_MAX, 2}, 17, entries));
  EXPECT_FALSE(wire::within({17, 1, 8, 3}, 17, entries));
  wire::read_query query;
  EXPECT_FALSE(wire::query(wire::query({17, 1, 0, fc::max_chunk_bytes + 1}), query));
  unsigned char buffer[2]; ULONG got = 0;
  EXPECT_EQ(wire::reply(wire::reply(S_OK, "x", 1), buffer, 2, got), STG_E_READFAULT);
  EXPECT_EQ(wire::reply(wire::reply(STG_E_REVERTED), buffer, 2, got), STG_E_REVERTED);
}
TEST(FileClipboard, RoutesRequirePairedIdentityAndLiveStream) {
  web::Server<nvhttp::SunshineHTTPS> server;
  file_clipboard::register_routes(server, [](std::string_view uuid) { return uuid == "paired-test"; });
  auto call = [&](std::string uuid) {
    auto request = std::make_shared<web::Request>(); request->method = "POST";
    request->path = "/api/v1/clipboard/files/open"; request->client_uuid = std::move(uuid);
    auto output = std::make_shared<std::string>();
    web::detail::ResponseChannel channel;
    channel.send = [output](std::string value, auto done, bool, bool) { *output += value; if (done) done({}); };
    channel.closed = [] { return false; }; channel.abort = [] {}; channel.begin_stream = [] {};
    auto response = std::make_shared<web::ServerBase<nvhttp::SunshineHTTPS>::Response>(*request, std::move(channel));
    server.resource.at("^/api/v1/clipboard/files/open$").at("POST")(response, request); response->send();
    return *output;
  };
  EXPECT_NE(call("").find("403"), std::string::npos);
  EXPECT_NE(call("unpaired-test").find("403"), std::string::npos);
  EXPECT_NE(call("paired-test").find("409"), std::string::npos);
}
#endif
