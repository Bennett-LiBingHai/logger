#include "logger/field.h"

#include <unordered_map>

#include "logger/utiils.h"

// 时间点 → ISO8601 字符串（本地时间 + 毫秒），JSON 下带引号
void encode(std::chrono::system_clock::time_point v, std::string& o, bool json) {
  std::string s = format_time_ms(v, LogConfig{});
  encode(s, o, json);
}

// 文本日志按行解析，多行堆栈必须折叠为单行；JSON 交给 json_escape 转义 \n
void encode(const StackTrace& v, std::string& o, bool json) {
  const std::string& s = v.str();
  if (json) {
    o += '"';
    o += json_escape(s);
    o += '"';
  } else {
    for (char c : s)
      o += (c == '\n') ? '|' : c;
  }
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

// 追加字段：空 key 跳过
void append_field(std::vector<Field>& fields, Field f) {
  if (!f.key.empty())
    fields.emplace_back(std::move(f));
}

// 字段去重：同 key 后写覆盖（保留最后一个值），位置取首次出现，保持顺序
void dedup_fields(std::vector<Field>& fields) {
  if (fields.size() < 2)
    return;
  std::vector<Field> out;
  out.reserve(fields.size());
  std::unordered_map<std::string, size_t> index;  // key → 在 out 中的位置
  for (auto& f : fields) {
    auto it = index.find(f.key);
    if (it == index.end()) {
      index.emplace(f.key, out.size());
      out.emplace_back(std::move(f));
    } else {
      out[it->second].value = std::move(f.value);  // 覆盖值，保留首次位置
    }
  }
  fields = std::move(out);
}
