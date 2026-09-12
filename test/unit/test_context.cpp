#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>

#include "logger/logger.h"

#include "test_helpers.h"

// M5：ContextScope —— thread_local 上下文栈，作用域内日志自动附字段
class ContextTest : public ::testing::Test {
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

TEST_F(ContextTest, AttachesFieldsInsideScope) {
  {
    ContextScope ctx{KV("request_id", "req-1"), KV("user_id", "u-9")};
    LOG_INFO("in scope");
  }
  const std::string line = only_message();
  EXPECT_NE(line.find("request_id=req-1"), std::string::npos);
  EXPECT_NE(line.find("user_id=u-9"), std::string::npos);
}

TEST_F(ContextTest, FieldsGoneAfterScope) {
  { ContextScope ctx{KV("request_id", "req-1")}; }  // 作用域结束，上下文字段随之出栈
  LOG_INFO("out of scope");
  const std::string line = only_message();
  EXPECT_EQ(line.find("request_id"), std::string::npos);
}

TEST_F(ContextTest, NestedScopesMergeInnerLast) {
  ContextScope outer{KV("layer", "outer"), KV("kept", "1")};
  {
    ContextScope inner{KV("layer", "inner")};
    LOG_INFO("nested");
  }
  const std::string line = only_message();
  EXPECT_NE(line.find("layer=inner"), std::string::npos);  // 内层在栈顶，后合并，覆盖外层
  EXPECT_EQ(line.find("layer=outer"), std::string::npos);
  EXPECT_NE(line.find("kept=1"), std::string::npos);  // 外层其它字段保留
}

TEST_F(ContextTest, InnerScopeDoesNotAffectOuterAfterExit) {
  ContextScope outer{KV("only_outer", "yes")};
  { ContextScope inner{KV("only_inner", "yes")}; }
  LOG_INFO("after inner");
  const std::string line = only_message();
  EXPECT_NE(line.find("only_outer=yes"), std::string::npos);
  EXPECT_EQ(line.find("only_inner"), std::string::npos);  // 内层已出栈
}

TEST_F(ContextTest, ExplicitKvOverridesContext) {
  ContextScope ctx{KV("env", "from-context")};
  Logger::get_instance().info("msg", KV("env", "explicit"));
  const std::string line = only_message();
  EXPECT_NE(line.find("env=explicit"), std::string::npos);
  EXPECT_EQ(line.find("env=from-context"), std::string::npos);
}

TEST_F(ContextTest, ContextOverridesWithPrebound) {
  // 优先级：显式 KV > ContextScope > with()（见 milestone「上下文支持」）
  ContextScope ctx{KV("svc", "from-context")};
  Logger::get_instance().with(KV("svc", "from-with")).info("msg");
  const std::string line = only_message();
  EXPECT_NE(line.find("svc=from-context"), std::string::npos);
  EXPECT_EQ(line.find("svc=from-with"), std::string::npos);
}

TEST_F(ContextTest, FullPrecedenceChain) {
  // 同一 key 三种来源同时出现：显式 KV 最高，上下文次之，with() 最低
  ContextScope ctx{KV("k", "context")};
  Logger::get_instance().with(KV("k", "with")).info("msg", KV("k", "explicit"));

  const std::string line = only_message();
  EXPECT_NE(line.find("k=explicit"), std::string::npos);
  EXPECT_EQ(line.find("k=context"), std::string::npos);
  EXPECT_EQ(line.find("k=with"), std::string::npos);
}

TEST_F(ContextTest, WorksWithJsonFormat) {
  LogConfig cfg;
  cfg.format = LogFormat::JSON;
  Logger::get_instance().set_config(cfg);

  ContextScope ctx{KV("trace_id", "abc")};
  LOG_INFO("hi");

  const std::string line = only_message();
  EXPECT_NE(line.find("\"trace_id\": \"abc\""), std::string::npos);
}

TEST_F(ContextTest, NotInheritedByChildThread) {
  // ContextScope 是 thread_local：子线程拿不到父线程的上下文
  ContextScope ctx{KV("request_id", "req-1")};
  std::thread child([] { LOG_INFO("from child"); });
  child.join();

  const std::string line = only_message();
  EXPECT_EQ(line.find("request_id"), std::string::npos);
}

TEST_F(ContextTest, SurvivesManyScopesWithoutImbalance) {
  // 反复进出作用域：栈必须完全平衡，不能残留字段或弹空
  for (int i = 0; i < 100; ++i) {
    ContextScope ctx{KV("iter", i)};
  }
  LOG_INFO("done");
  const std::string line = only_message();
  EXPECT_EQ(line.find("iter="), std::string::npos);
}
