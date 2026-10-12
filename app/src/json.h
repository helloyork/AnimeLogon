// JSON for the messages between the settings app's page and its native side. WebView2 hands
// both directions over as UTF-16 text, so strings stay UTF-16 here as well.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsString() const { return type_ == Type::String; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    // Missing members and items, and members of anything but an object, read as null.
    const Value &operator[](std::wstring_view key) const;
    const Value &operator[](size_t index) const;
    size_t size() const { return type_ == Type::Array ? items_.size() : 0; }

    // The value, or `fallback` when it is of another type.
    std::wstring Str(const std::wstring &fallback = L"") const { return type_ == Type::String ? text_ : fallback; }
    double Num(double fallback = 0) const { return type_ == Type::Number ? number_ : fallback; }
    bool Bool(bool fallback = false) const { return type_ == Type::Bool ? flag_ : fallback; }
    const std::vector<Value> &Items() const { return items_; }
    const std::map<std::wstring, Value, std::less<>> &Members() const { return members_; }

private:
    friend class Parser;
    Type type_ = Type::Null;
    bool flag_ = false;
    double number_ = 0;
    std::wstring text_;
    std::vector<Value> items_;
    std::map<std::wstring, Value, std::less<>> members_;
};

// Strict RFC 8259 JSON, nested at most 32 deep. False on anything else.
bool Parse(std::wstring_view text, Value *out);

// Writes JSON text. Members and items are separated as they are added.
class Writer {
public:
    Writer &BeginObject();
    Writer &EndObject();
    Writer &BeginArray();
    Writer &EndArray();
    Writer &Key(std::wstring_view key);
    Writer &String(std::wstring_view value);
    Writer &Number(double value);
    Writer &Int(long long value);
    Writer &Bool(bool value);
    Writer &Null();
    // Shorthands for a member.
    Writer &Member(std::wstring_view key, std::wstring_view value) { return Key(key).String(value); }
    Writer &Member(std::wstring_view key, const wchar_t *value) { return Key(key).String(value); }
    Writer &Member(std::wstring_view key, bool value) { return Key(key).Bool(value); }
    Writer &Member(std::wstring_view key, int value) { return Key(key).Int(value); }
    Writer &Member(std::wstring_view key, double value) { return Key(key).Number(value); }

    const std::wstring &text() const { return out_; }

private:
    void Separate();
    std::wstring out_;
    bool first_ = true;
};

}  // namespace json
