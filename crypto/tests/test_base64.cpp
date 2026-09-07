// test_base64.cpp —— Base64 模块单元测试
//
// 验证点：
//   1. RFC 4648 官方测试向量
//   2. 全字节 0x00~0xFF 往返一致性
//   3. 随机二进制数据往返一致性
//   4. 带换行编码可被标准解码器还原
//   5. 非法输入返回空
//
// 编译运行（WSL/Linux）：
//   g++ -std=c++17 -I. tests/test_base64.cpp common/base64.cpp -o tests/test_base64
//   ./tests/test_base64
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "common/base64.hpp"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("[FAIL] %s\n", what);
    ++g_failures;
  } else {
    std::printf("[ OK ] %s\n", what);
  }
}

void TestRfc4648Vectors() {
  // RFC 4648 §10 官方测试向量
  Check(mail::Base64Encode(std::string("")) == "",
        "RFC向量: \"\" -> \"\"");
  Check(mail::Base64Encode(std::string("f")) == "Zg==",
        "RFC向量: \"f\" -> \"Zg==\"");
  Check(mail::Base64Encode(std::string("fo")) == "Zm8=",
        "RFC向量: \"fo\" -> \"Zm8=\"");
  Check(mail::Base64Encode(std::string("foo")) == "Zm9v",
        "RFC向量: \"foo\" -> \"Zm9v\"");
  Check(mail::Base64Encode(std::string("foob")) == "Zm9vYg==",
        "RFC向量: \"foob\" -> \"Zm9vYg==\"");
  Check(mail::Base64Encode(std::string("fooba")) == "Zm9vYmE=",
        "RFC向量: \"fooba\" -> \"Zm9vYmE=\"");
  Check(mail::Base64Encode(std::string("foobar")) == "Zm9vYmFy",
        "RFC向量: \"foobar\" -> \"Zm9vYmFy\"");
}

void TestRoundTripAllBytes() {
  std::vector<unsigned char> data(256);
  for (int i = 0; i < 256; ++i) data[i] = static_cast<unsigned char>(i);

  const std::string b64 = mail::Base64Encode(data.data(), data.size());
  const auto decoded = mail::Base64Decode(b64);
  Check(decoded == data, "0x00~0xFF 全字节往返一致");
}

void TestRoundTripRandom() {
  std::mt19937 rng(20260902);
  for (std::size_t len : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 63u, 64u, 65u,
                          1000u, 1024u, 4096u}) {
    std::vector<unsigned char> data(len);
    for (auto& b : data) b = static_cast<unsigned char>(rng() & 0xFF);

    const std::string b64 = mail::Base64Encode(data.data(), data.size());
    const auto decoded = mail::Base64Decode(b64);
    const std::string label = "随机数据往返 len=" + std::to_string(len);
    Check(decoded == data, label.c_str());
  }
}

void TestWrappedEncode() {
  std::string plain(200, 'x');
  const std::string wrapped = mail::Base64EncodeWrapped(
      reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), 76);

  // 每行不超过 76 字符（最后一行除外），且存在换行
  bool has_newline = false;
  std::size_t line_start = 0;
  while (line_start < wrapped.size()) {
    const std::size_t nl = wrapped.find('\n', line_start);
    const std::size_t line_end = (nl == std::string::npos) ? wrapped.size() : nl;
    if (line_end - line_start > 76) {
      Check(false, "带换行编码：某行超过 76 字符");
      return;
    }
    if (nl != std::string::npos) has_newline = true;
    line_start = line_end + 1;
  }
  Check(has_newline, "带换行编码：确实插入了换行");
  Check(mail::Base64DecodeToString(wrapped) == plain,
        "带换行编码可被解码器还原");
}

void TestInvalidInput() {
  Check(mail::Base64Decode("!!!not-base64!!!").empty(), "非法字符返回空");
  Check(mail::Base64Decode("AAAA=A").empty(), "'=' 后出现字符返回空");
}

}  // namespace

int main() {
  std::printf("===== Base64 单元测试 =====\n");
  TestRfc4648Vectors();
  TestRoundTripAllBytes();
  TestRoundTripRandom();
  TestWrappedEncode();
  TestInvalidInput();

  if (g_failures == 0) {
    std::printf("===== 全部通过 =====\n");
    return 0;
  }
  std::printf("===== 失败 %d 项 =====\n", g_failures);
  return 1;
}
