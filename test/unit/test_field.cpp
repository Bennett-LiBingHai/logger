#include <gtest/gtest.h>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

#include "logger/field.h"

namespace {
struct Streamable {
  int x;
};
std::ostream& operator<<(std::ostream& os, const Streamable& s) {
  return os << "S(" << s.x << ")";
}
}  // namespace

// M2：encode 各类型 + Field 类型保留
TEST(EncodeTest, EncodesIntAsNumber) {
  EXPECT_EQ(encode(2001, false), "2001");
  EXPECT_EQ(encode(2001, true), "2001");  // 数字不加引号
}

TEST(EncodeTest, EncodesDoubleShortest) {
  EXPECT_EQ(encode(99.5, false), "99.5");
  EXPECT_EQ(encode(99.5, true), "99.5");
}

TEST(EncodeTest, EncodesBool) {
  EXPECT_EQ(encode(true, false), "true");
  EXPECT_EQ(encode(false, true), "false");
}

TEST(EncodeTest, EncodesStringWithJsonQuoting) {
  EXPECT_EQ(encode(std::string("hi"), false), "hi");
  EXPECT_EQ(encode(std::string("hi"), true), "\"hi\"");
}

TEST(EncodeTest, EncodesNullCStr) {
  const char* p = nullptr;
  EXPECT_EQ(encode(p, true), "null");
  EXPECT_EQ(encode(p, false), "(null)");
}

TEST(EncodeTest, EncodesVector) {
  std::vector<int> v{1, 2, 3};
  EXPECT_EQ(encode(v, false), "[1, 2, 3]");
  EXPECT_EQ(encode(v, true), "[1,2,3]");
}

TEST(EncodeTest, EncodesEmptyVector) {
  std::vector<int> v;
  EXPECT_EQ(encode(v, false), "[]");
  EXPECT_EQ(encode(v, true), "[]");
}

TEST(EncodeTest, EncodesCustomStreamable) {
  Streamable s{7};
  EXPECT_EQ(encode(s, false), "S(7)");
  EXPECT_EQ(encode(s, true), "\"S(7)\"");
}

TEST(EncodeTest, EncodesNanInf) {
  double nan = std::numeric_limits<double>::quiet_NaN();
  double inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(encode(nan, false), "nan");
  EXPECT_EQ(encode(nan, true), "null");
  EXPECT_EQ(encode(inf, true), "null");
  EXPECT_EQ(encode(-inf, false), "-inf");
}

TEST(FieldValueTest, PreservesTypedValueAndCopies) {
  Field f = KV("user_id", 2001);
  EXPECT_EQ(f.key, "user_id");

  std::string text;
  std::string json;
  f.value.encode(text, false);
  f.value.encode(json, true);
  EXPECT_EQ(text, "2001");
  EXPECT_EQ(json, "2001");

  Field f2 = f;  // 拷贝
  std::string j2;
  f2.value.encode(j2, true);
  EXPECT_EQ(j2, "2001");
}

TEST(KVTest, StringLiteralKey) {
  Field f = KV("order_id", "ORD-1001");
  std::string j;
  f.value.encode(j, true);
  EXPECT_EQ(j, "\"ORD-1001\"");
}
