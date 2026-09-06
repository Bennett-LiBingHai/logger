#include "logger/field.h"

#include "logger/utiils.h"

// 时间点 → ISO8601 字符串（本地时间 + 毫秒），JSON 下带引号
void encode(std::chrono::system_clock::time_point v, std::string& o, bool json) {
  std::string s = format_time_ms(v, LogConfig{});
  encode(s, o, json);
}

FieldValue::FieldValue(FieldValue&& o) noexcept {
  swap(o);
}
FieldValue& FieldValue::operator=(FieldValue&& o) noexcept {
  swap(o);
  return *this;
}
FieldValue::FieldValue(const FieldValue& o) {
  if (o.clone_) {
    obj_ = o.clone_(o.obj_);
    encode_ = o.encode_;
    clone_ = o.clone_;
    destroy_ = o.destroy_;
  }
}
FieldValue& FieldValue::operator=(const FieldValue& o) {
  FieldValue t(o);
  swap(t);
  return *this;
}
// 析构函数
FieldValue::~FieldValue() {
  if (obj_) {
    destroy_(obj_);
  }
}

// 追加当前值的字符串编码到out
void FieldValue::encode(std::string& out, bool json) const {
  if (encode_)
    encode_(obj_, out, json);
}

// 交换资源
void FieldValue::swap(FieldValue& o) noexcept {
  using std::swap;
  swap(obj_, o.obj_);
  swap(encode_, o.encode_);
  swap(clone_, o.clone_);
  swap(destroy_, o.destroy_);
}
