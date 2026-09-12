#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "logger/logger.h"
#include "logger/trace.h"

#include "test_helpers.h"

// M5：Trace 系统集成 —— traceparent 编解码与字段挂载

namespace {

constexpr const char* kValidTraceId = "4bf92f3577b34da6a3ce929d0e0e4736";
constexpr const char* kValidSpanId = "00f067aa0ba902b7";

// 全为合法 hex 的 n 位串
bool is_hex_of_len(const std::string& s, std::size_t n) {
  if (s.size() != n)
    return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  return true;
}

bool all_zero(const std::string& s) {
  for (char c : s)
    if (c != '0')
      return false;
  return true;
}

}  // namespace

TEST(TraceContextTest, DefaultsAreApplied) {
  const TraceContext ctx;
  EXPECT_EQ(ctx.version, "00");
  EXPECT_EQ(ctx.trace_flags, "01");
  EXPECT_TRUE(ctx.trace_id.empty());  // 必须由调用方填
  EXPECT_TRUE(ctx.sampled());         // 默认 flags "01"，bit0 = 1，即采样
}

TEST(GenerateTraceTest, ProducesWellFormedNonZeroIds) {
  for (int i = 0; i < 64; ++i) {
    const TraceContext ctx = generate_trace();
    EXPECT_TRUE(is_hex_of_len(ctx.trace_id, 32));
    EXPECT_TRUE(is_hex_of_len(ctx.span_id, 16));
    EXPECT_FALSE(all_zero(ctx.trace_id));  // W3C 禁止全 0
    EXPECT_FALSE(all_zero(ctx.span_id));
    EXPECT_EQ(ctx.version, "00");
    EXPECT_EQ(ctx.trace_flags, "01");
    EXPECT_TRUE(ctx.sampled());
  }
}

TEST(GenerateTraceTest, IdsDifferAcrossCalls) {
  const TraceContext a = generate_trace();
  const TraceContext b = generate_trace();
  EXPECT_NE(a.trace_id, b.trace_id);
  EXPECT_NE(a.span_id, b.span_id);
}

TEST(ParseTraceparentTest, ParsesValidHeader) {
  TraceContext ctx;
  ASSERT_TRUE(
      parse_traceparent(std::string("00-") + kValidTraceId + "-" + kValidSpanId + "-01", ctx));
  EXPECT_EQ(ctx.version, "00");
  EXPECT_EQ(ctx.trace_id, kValidTraceId);
  EXPECT_EQ(ctx.span_id, kValidSpanId);
  EXPECT_EQ(ctx.trace_flags, "01");
  EXPECT_TRUE(ctx.sampled());
}

TEST(ParseTraceparentTest, RoundTripsGeneratedContext) {
  const TraceContext original = generate_trace();
  TraceContext parsed;
  ASSERT_TRUE(parse_traceparent(make_traceparent(original), parsed));
  EXPECT_EQ(parsed.trace_id, original.trace_id);
  EXPECT_EQ(parsed.span_id, original.span_id);
  EXPECT_EQ(parsed.trace_flags, original.trace_flags);
  EXPECT_EQ(parsed.version, original.version);
}

TEST(ParseTraceparentTest, RejectsMalformedHeaders) {
  const std::string base = std::string("00-") + kValidTraceId + "-" + kValidSpanId + "-01";
  const std::string bad[] = {
      "",
      "00",
      "00-" + std::string(kValidTraceId),                                       // 段数不足
      std::string("00-") + kValidTraceId + "-" + kValidSpanId,                  // 缺 flags
      "00-" + std::string(32, '0') + "-" + kValidSpanId + "-01",                // trace_id 全 0
      "00-" + std::string(kValidTraceId) + "-" + std::string(16, '0') + "-01",  // span_id 全 0
      base + "-extra",  // version 00 不得有追加字段
      "zz-" + std::string(kValidTraceId) + "-" + kValidSpanId + "-01",  // version 非 hex
      "0-" + std::string(kValidTraceId) + "-" + kValidSpanId + "-01",   // version 长度错
      "00-" + std::string(31, 'a') + "-" + kValidSpanId + "-01",        // trace_id 长度错
      "00-" + std::string(kValidTraceId) + "-" + std::string(15, 'b') + "-01",  // span_id 长度错
      "00-" + std::string(kValidTraceId) + "-" + kValidSpanId + "-0",           // flags 长度错
      "00-" + std::string(kValidTraceId) + "-" + kValidSpanId + "-zz",          // flags 非 hex
      "00-" + std::string(kValidTraceId) + "-" + kValidSpanId + "-0z",          // flags 半非法
  };
  TraceContext ctx;
  for (const std::string& header : bad) {
    EXPECT_FALSE(parse_traceparent(header, ctx)) << "should reject: " << header;
  }
}

TEST(ParseTraceparentTest, AcceptsHigherVersionAndIgnoresExtraFields) {
  // W3C：version != 00 时，前 4 段照常解析，追加字段忽略
  TraceContext ctx;
  const std::string header = std::string("01-") + kValidTraceId + "-" + kValidSpanId + "-01-abcdef";
  ASSERT_TRUE(parse_traceparent(header, ctx));
  EXPECT_EQ(ctx.version, "01");
  EXPECT_EQ(ctx.trace_id, kValidTraceId);
  EXPECT_EQ(ctx.span_id, kValidSpanId);
}

TEST(MakeTraceparentTest, FormatsAllFourSegments) {
  TraceContext ctx;
  ctx.trace_id = kValidTraceId;
  ctx.span_id = kValidSpanId;
  ctx.trace_flags = "01";
  EXPECT_EQ(make_traceparent(ctx), std::string("00-") + kValidTraceId + "-" + kValidSpanId + "-01");
}

TEST(MakeTraceparentTest, PreservesVersionAndFlagsVerbatim) {
  TraceContext ctx;
  ctx.version = "01";
  ctx.trace_id = kValidTraceId;
  ctx.span_id = kValidSpanId;
  ctx.trace_flags = "03";
  EXPECT_EQ(make_traceparent(ctx), std::string("01-") + kValidTraceId + "-" + kValidSpanId + "-03");
}

TEST(MakeTraceparentTest, RejectsAnyInvalidFieldWithoutSubstituting) {
  // 四个字段一视同仁：任一非法即空串，绝不悄悄替换成默认值
  auto valid_ctx = [] {
    TraceContext ctx;
    ctx.trace_id = kValidTraceId;
    ctx.span_id = kValidSpanId;
    return ctx;
  };

  TraceContext bad_version = valid_ctx();
  bad_version.version = "zz";
  EXPECT_TRUE(make_traceparent(bad_version).empty());

  TraceContext bad_trace = valid_ctx();
  bad_trace.trace_id = std::string(31, 'a');
  EXPECT_TRUE(make_traceparent(bad_trace).empty());

  TraceContext zero_trace = valid_ctx();
  zero_trace.trace_id = std::string(32, '0');
  EXPECT_TRUE(make_traceparent(zero_trace).empty());

  TraceContext bad_span = valid_ctx();
  bad_span.span_id = std::string(16, '0');
  EXPECT_TRUE(make_traceparent(bad_span).empty());

  TraceContext bad_flags = valid_ctx();
  bad_flags.trace_flags = "0x";
  EXPECT_TRUE(make_traceparent(bad_flags).empty());  // 不替换为 "00"

  TraceContext empty_ctx;  // trace_id 未填
  EXPECT_TRUE(make_traceparent(empty_ctx).empty());
}

TEST(SampledTest, UsesBit0NotWholeNibble) {
  TraceContext ctx;
  ctx.trace_flags = "00";
  EXPECT_FALSE(ctx.sampled());
  ctx.trace_flags = "01";
  EXPECT_TRUE(ctx.sampled());
  ctx.trace_flags = "02";  // bit0 = 0 → 未采样
  EXPECT_FALSE(ctx.sampled());
  ctx.trace_flags = "0c";  // bit0 = 0 → 未采样
  EXPECT_FALSE(ctx.sampled());
  ctx.trace_flags = "03";  // bit0 = 1 → 采样
  EXPECT_TRUE(ctx.sampled());
  ctx.trace_flags = "ff";
  EXPECT_TRUE(ctx.sampled());
}

TEST(SampledTest, MalformedFlagsAreNotSampled) {
  TraceContext ctx;
  for (const char* flags : {"", "0", "z1", "1z", "zz", "001"}) {
    ctx.trace_flags = flags;
    EXPECT_FALSE(ctx.sampled()) << "flags: " << flags;
  }
}

// ===== 字段挂载 =====

class TraceLogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Logger::get_instance().set_config(LogConfig{});
    sink_ = std::make_shared<CapturingSink>();
    Logger::get_instance().add_sink(sink_);
  }

  std::string only_message() {
    EXPECT_EQ(sink_->size(), 1u);
    return sink_->messages().empty() ? std::string{} : sink_->messages().front();
  }

  std::shared_ptr<CapturingSink> sink_;
};

TEST_F(TraceLogTest, WithTraceAttachesAllFields) {
  TraceContext ctx;
  ctx.trace_id = kValidTraceId;
  ctx.span_id = kValidSpanId;
  ctx.trace_flags = "01";

  Logger::get_instance().with_trace(ctx).info("payment failed");

  const std::string line = only_message();
  EXPECT_NE(line.find(std::string("trace_id=") + kValidTraceId), std::string::npos);
  EXPECT_NE(line.find(std::string("span_id=") + kValidSpanId), std::string::npos);
  EXPECT_NE(line.find("trace_flags=01"), std::string::npos);
}

TEST_F(TraceLogTest, ContextScopeAttachesTraceFields) {
  const TraceContext ctx = generate_trace();
  {
    ContextScope scope{KV("trace_id", ctx.trace_id), KV("span_id", ctx.span_id),
                       KV("trace_flags", ctx.trace_flags)};
    LOG_INFO("handled");
  }
  const std::string line = only_message();
  EXPECT_NE(line.find("trace_id=" + ctx.trace_id), std::string::npos);
  EXPECT_NE(line.find("span_id=" + ctx.span_id), std::string::npos);
}

TEST_F(TraceLogTest, BusinessFieldHelpersUseFixedNames) {
  Logger::get_instance()
      .with_request_id("req-1")
      .with_user_id("user-2")
      .with_service("order-service")
      .info("order created");

  const std::string line = only_message();
  EXPECT_NE(line.find("request_id=req-1"), std::string::npos);
  EXPECT_NE(line.find("user_id=user-2"), std::string::npos);
  EXPECT_NE(line.find("service=order-service"), std::string::npos);
}

TEST_F(TraceLogTest, BusinessFieldHelpersChainAndOverride) {
  Logger::get_instance().with_service("a").with_service("b").info("msg");
  const std::string line = only_message();
  EXPECT_NE(line.find("service=b"), std::string::npos);
  EXPECT_EQ(line.find("service=a"), std::string::npos);
}
