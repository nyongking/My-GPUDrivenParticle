#pragma once
// 작은 JSON 읽기 / 쓰기 (이미터 에셋 저장용)
//   객체 키 순서를 유지한다 (저장 파일 diff 가 안정적이도록)
//   숫자는 double 하나로 다룬다. float 로 만든 값은 다시 읽어도 같은 float 가 되는 가장 짧은 자리수로 쓴다 (0.1 -> 0.1)

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class JsonValue
{
public:
	enum class Type { Null, Bool, Number, String, Array, Object };
	using Member = std::pair<std::string, JsonValue>;

	JsonValue() = default;
	explicit JsonValue(bool v) : m_Type(Type::Bool), m_Bool(v) {}
	explicit JsonValue(int v) : m_Type(Type::Number), m_Number(double(v)) {}
	explicit JsonValue(uint32_t v) : m_Type(Type::Number), m_Number(double(v)) {}
	explicit JsonValue(float v) : m_Type(Type::Number), m_Number(double(v)), m_FromFloat(true) {}
	explicit JsonValue(double v) : m_Type(Type::Number), m_Number(v) {}
	explicit JsonValue(const char* v) : m_Type(Type::String), m_String(v) {}
	explicit JsonValue(const std::string& v) : m_Type(Type::String), m_String(v) {}

	static JsonValue MakeArray();
	static JsonValue MakeObject();

	Type GetType() const { return m_Type; }
	bool IsNull() const { return m_Type == Type::Null; }
	bool IsBool() const { return m_Type == Type::Bool; }
	bool IsNumber() const { return m_Type == Type::Number; }
	bool IsString() const { return m_Type == Type::String; }
	bool IsArray() const { return m_Type == Type::Array; }
	bool IsObject() const { return m_Type == Type::Object; }

	bool               AsBool(bool fallback = false) const { return IsBool() ? m_Bool : fallback; }
	double             AsNumber(double fallback = 0.0) const { return IsNumber() ? m_Number : fallback; }
	const std::string& AsString() const { return m_String; }   // 문자열이 아니면 빈 문자열
	bool               FromFloat() const { return m_FromFloat; }

	// 배열
	size_t           Size() const { return m_Array.size(); }
	const JsonValue& At(size_t i) const { return m_Array[i]; }
	JsonValue&       Push(JsonValue value);

	// 객체 (같은 키가 있으면 덮어쓴다)
	const JsonValue*           Find(const std::string& key) const;
	const std::vector<Member>& Members() const { return m_Object; }
	JsonValue&                 Set(const std::string& key, JsonValue value);

private:
	Type                   m_Type = Type::Null;
	bool                   m_Bool = false;
	double                 m_Number = 0.0;
	bool                   m_FromFloat = false;   // 쓰기 자리수용
	std::string            m_String;
	std::vector<JsonValue> m_Array;
	std::vector<Member>    m_Object;
};

// 실패하면 false + error 에 "line N: 이유" (영어, ImGui 에 그대로 보여 준다)
bool        ParseJson(const std::string& text, JsonValue& out, std::string& error);
// 들여쓰기 2칸. 숫자 / bool 만 있는 배열은 한 줄로 쓴다
std::string WriteJson(const JsonValue& value);
