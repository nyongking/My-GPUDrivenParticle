#include "pch.h"
#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

JsonValue JsonValue::MakeArray()
{
	JsonValue v;
	v.m_Type = Type::Array;
	return v;
}

JsonValue JsonValue::MakeObject()
{
	JsonValue v;
	v.m_Type = Type::Object;
	return v;
}

JsonValue& JsonValue::Push(JsonValue value)
{
	m_Type = Type::Array;
	m_Array.push_back(std::move(value));
	return m_Array.back();
}

const JsonValue* JsonValue::Find(const std::string& key) const
{
	if (m_Type != Type::Object)
		return nullptr;
	for (const Member& m : m_Object)
		if (m.first == key)
			return &m.second;
	return nullptr;
}

JsonValue& JsonValue::Set(const std::string& key, JsonValue value)
{
	m_Type = Type::Object;
	for (Member& m : m_Object)
	{
		if (m.first == key)
		{
			m.second = std::move(value);
			return m.second;
		}
	}
	m_Object.emplace_back(key, std::move(value));
	return m_Object.back().second;
}

// ---------------------------------------------------------------- 읽기

namespace
{
	class Parser
	{
	public:
		explicit Parser(const std::string& text) : m_Text(text) {}

		bool Run(JsonValue& out, std::string& error)
		{
			SkipSpace();
			if (!ParseValue(out, 0))
			{
				error = m_Error;
				return false;
			}
			SkipSpace();
			if (m_Pos != m_Text.size())
			{
				Fail("unexpected text after the root value");
				error = m_Error;
				return false;
			}
			return true;
		}

	private:
		static const int kMaxDepth = 64;   // 깨진 파일로 스택이 넘치지 않게

		bool Fail(const char* why)
		{
			if (!m_Error.empty())
				return false;   // 가장 안쪽 이유만 남긴다
			int line = 1;
			for (size_t i = 0; i < m_Pos && i < m_Text.size(); ++i)
				if (m_Text[i] == '\n')
					++line;
			m_Error = "line " + std::to_string(line) + ": " + why;
			return false;
		}

		bool AtEnd() const { return m_Pos >= m_Text.size(); }
		char Peek() const { return AtEnd() ? '\0' : m_Text[m_Pos]; }

		void SkipSpace()
		{
			while (!AtEnd())
			{
				const char c = m_Text[m_Pos];
				if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
					++m_Pos;
				else
					break;
			}
		}

		bool Expect(const char* word)
		{
			for (const char* w = word; *w; ++w, ++m_Pos)
				if (Peek() != *w)
					return Fail("invalid literal");
			return true;
		}

		bool ParseValue(JsonValue& out, int depth)
		{
			if (depth > kMaxDepth)
				return Fail("nesting too deep");

			switch (Peek())
			{
			case '{': return ParseObject(out, depth);
			case '[': return ParseArray(out, depth);
			case '"':
			{
				std::string s;
				if (!ParseString(s))
					return false;
				out = JsonValue(s);
				return true;
			}
			case 't': if (!Expect("true")) return false;  out = JsonValue(true);  return true;
			case 'f': if (!Expect("false")) return false; out = JsonValue(false); return true;
			case 'n': if (!Expect("null")) return false;  out = JsonValue();      return true;
			default:  return ParseNumber(out);
			}
		}

		bool ParseNumber(JsonValue& out)
		{
			const size_t start = m_Pos;
			while (!AtEnd())
			{
				const char c = m_Text[m_Pos];
				if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
					++m_Pos;
				else
					break;
			}
			if (m_Pos == start)
				return Fail("unexpected character");

			// strtod 는 C 로캘 기준 ('.' 소수점). 이 프로젝트는 setlocale 을 바꾸지 않는다
			const std::string token = m_Text.substr(start, m_Pos - start);
			char* end = nullptr;
			const double v = std::strtod(token.c_str(), &end);
			if (end != token.c_str() + token.size())
			{
				m_Pos = start;
				return Fail("invalid number");
			}
			out = JsonValue(v);
			return true;
		}

		static void AppendUtf8(std::string& s, uint32_t cp)
		{
			if (cp < 0x80)
				s += char(cp);
			else if (cp < 0x800)
			{
				s += char(0xC0 | (cp >> 6));
				s += char(0x80 | (cp & 0x3F));
			}
			else if (cp < 0x10000)
			{
				s += char(0xE0 | (cp >> 12));
				s += char(0x80 | ((cp >> 6) & 0x3F));
				s += char(0x80 | (cp & 0x3F));
			}
			else
			{
				s += char(0xF0 | (cp >> 18));
				s += char(0x80 | ((cp >> 12) & 0x3F));
				s += char(0x80 | ((cp >> 6) & 0x3F));
				s += char(0x80 | (cp & 0x3F));
			}
		}

		bool ParseHex4(uint32_t& out)
		{
			out = 0;
			for (int i = 0; i < 4; ++i, ++m_Pos)
			{
				const char c = Peek();
				uint32_t d = 0;
				if (c >= '0' && c <= '9')      d = uint32_t(c - '0');
				else if (c >= 'a' && c <= 'f') d = uint32_t(c - 'a' + 10);
				else if (c >= 'A' && c <= 'F') d = uint32_t(c - 'A' + 10);
				else return Fail("invalid \\u escape");
				out = (out << 4) | d;
			}
			return true;
		}

		bool ParseString(std::string& out)
		{
			++m_Pos;   // "
			while (true)
			{
				if (AtEnd())
					return Fail("unterminated string");
				const char c = m_Text[m_Pos++];
				if (c == '"')
					return true;
				if (c != '\\')
				{
					out += c;   // UTF-8 바이트는 그대로
					continue;
				}

				const char e = Peek();
				++m_Pos;
				switch (e)
				{
				case '"':  out += '"';  break;
				case '\\': out += '\\'; break;
				case '/':  out += '/';  break;
				case 'b':  out += '\b'; break;
				case 'f':  out += '\f'; break;
				case 'n':  out += '\n'; break;
				case 'r':  out += '\r'; break;
				case 't':  out += '\t'; break;
				case 'u':
				{
					uint32_t cp = 0;
					if (!ParseHex4(cp))
						return false;
					// 서로게이트 쌍
					if (cp >= 0xD800 && cp <= 0xDBFF && Peek() == '\\' && m_Pos + 1 < m_Text.size() && m_Text[m_Pos + 1] == 'u')
					{
						m_Pos += 2;
						uint32_t lo = 0;
						if (!ParseHex4(lo))
							return false;
						if (lo >= 0xDC00 && lo <= 0xDFFF)
							cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					}
					AppendUtf8(out, cp);
					break;
				}
				default:
					return Fail("invalid escape in string");
				}
			}
		}

		bool ParseArray(JsonValue& out, int depth)
		{
			++m_Pos;   // [
			out = JsonValue::MakeArray();
			SkipSpace();
			if (Peek() == ']')
			{
				++m_Pos;
				return true;
			}
			while (true)
			{
				JsonValue item;
				SkipSpace();
				if (!ParseValue(item, depth + 1))
					return false;
				out.Push(std::move(item));
				SkipSpace();
				const char c = Peek();
				++m_Pos;
				if (c == ']')
					return true;
				if (c != ',')
				{
					--m_Pos;
					return Fail("expected ',' or ']'");
				}
			}
		}

		bool ParseObject(JsonValue& out, int depth)
		{
			++m_Pos;   // {
			out = JsonValue::MakeObject();
			SkipSpace();
			if (Peek() == '}')
			{
				++m_Pos;
				return true;
			}
			while (true)
			{
				SkipSpace();
				if (Peek() != '"')
					return Fail("expected a key string");
				std::string key;
				if (!ParseString(key))
					return false;
				SkipSpace();
				if (Peek() != ':')
					return Fail("expected ':'");
				++m_Pos;
				SkipSpace();
				JsonValue value;
				if (!ParseValue(value, depth + 1))
					return false;
				out.Set(key, std::move(value));
				SkipSpace();
				const char c = Peek();
				++m_Pos;
				if (c == '}')
					return true;
				if (c != ',')
				{
					--m_Pos;
					return Fail("expected ',' or '}'");
				}
			}
		}

		const std::string& m_Text;
		size_t             m_Pos = 0;
		std::string        m_Error;
	};

	// ------------------------------------------------------------ 쓰기

	void WriteNumber(std::string& out, double v, bool fromFloat)
	{
		if (!std::isfinite(v))
		{
			out += "0";   // JSON 에는 NaN / Inf 가 없다
			return;
		}
		char buf[32];
		if (v == std::floor(v) && std::fabs(v) < 1e15)
		{
			std::snprintf(buf, sizeof(buf), "%.0f", v);
		}
		else
		{
			// 다시 읽었을 때 같은 값이 되는 가장 짧은 자리수 (float 는 최대 9, double 은 17)
			const int maxDigits = fromFloat ? 9 : 17;
			for (int digits = 1; digits <= maxDigits; ++digits)
			{
				std::snprintf(buf, sizeof(buf), "%.*g", digits, v);
				const double back = std::strtod(buf, nullptr);
				if (fromFloat ? (float(back) == float(v)) : (back == v))
					break;
			}
		}
		out += buf;
	}

	void WriteString(std::string& out, const std::string& s)
	{
		out += '"';
		for (const char ch : s)
		{
			const unsigned char c = static_cast<unsigned char>(ch);
			switch (c)
			{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (c < 0x20)
				{
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c));
					out += buf;
				}
				else
					out += ch;
			}
		}
		out += '"';
	}

	bool IsScalarArray(const JsonValue& v)
	{
		for (size_t i = 0; i < v.Size(); ++i)
			if (v.At(i).IsArray() || v.At(i).IsObject() || v.At(i).IsString())
				return false;
		return true;
	}

	void Indent(std::string& out, int level)
	{
		out.append(size_t(level) * 2, ' ');
	}

	void WriteValue(std::string& out, const JsonValue& v, int level)
	{
		switch (v.GetType())
		{
		case JsonValue::Type::Null:   out += "null"; break;
		case JsonValue::Type::Bool:   out += v.AsBool() ? "true" : "false"; break;
		case JsonValue::Type::Number: WriteNumber(out, v.AsNumber(), v.FromFloat()); break;
		case JsonValue::Type::String: WriteString(out, v.AsString()); break;
		case JsonValue::Type::Array:
			if (v.Size() == 0)
			{
				out += "[]";
			}
			else if (IsScalarArray(v))
			{
				out += '[';
				for (size_t i = 0; i < v.Size(); ++i)
				{
					if (i > 0)
						out += ", ";
					WriteValue(out, v.At(i), level);
				}
				out += ']';
			}
			else
			{
				out += "[\n";
				for (size_t i = 0; i < v.Size(); ++i)
				{
					Indent(out, level + 1);
					WriteValue(out, v.At(i), level + 1);
					out += (i + 1 < v.Size()) ? ",\n" : "\n";
				}
				Indent(out, level);
				out += ']';
			}
			break;
		case JsonValue::Type::Object:
		{
			const std::vector<JsonValue::Member>& members = v.Members();
			if (members.empty())
			{
				out += "{}";
				break;
			}
			out += "{\n";
			for (size_t i = 0; i < members.size(); ++i)
			{
				Indent(out, level + 1);
				WriteString(out, members[i].first);
				out += ": ";
				WriteValue(out, members[i].second, level + 1);
				out += (i + 1 < members.size()) ? ",\n" : "\n";
			}
			Indent(out, level);
			out += '}';
			break;
		}
		}
	}
}

bool ParseJson(const std::string& text, JsonValue& out, std::string& error)
{
	Parser parser(text);
	return parser.Run(out, error);
}

std::string WriteJson(const JsonValue& value)
{
	std::string out;
	WriteValue(out, value, 0);
	out += '\n';
	return out;
}
