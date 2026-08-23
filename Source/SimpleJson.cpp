#include "SimpleJson.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace cld
{
namespace
{
constexpr std::size_t MaximumJsonContainerDepth = 64;

std::uint32_t HexDigit(unsigned char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10u;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10u;
    }
    throw std::runtime_error("Invalid unicode escape in JSON string.");
}

std::uint32_t ParseUnicodeCodeUnit(
    const std::string& text,
    std::size_t& position)
{
    if (position > text.size() || text.size() - position < 4)
    {
        throw std::runtime_error("Invalid unicode escape in JSON string.");
    }
    std::uint32_t codeUnit = 0;
    for (int index = 0; index < 4; ++index)
    {
        codeUnit = (codeUnit << 4u) |
            HexDigit(static_cast<unsigned char>(text[position++]));
    }
    return codeUnit;
}

void AppendUtf8(std::uint32_t codePoint, std::string& output)
{
    if (codePoint <= 0x7fu)
    {
        output.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint <= 0x7ffu)
    {
        output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
    else if (codePoint <= 0xffffu)
    {
        output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
        output.push_back(static_cast<char>(
            0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
    else if (codePoint <= 0x10ffffu)
    {
        output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
        output.push_back(static_cast<char>(
            0x80u | ((codePoint >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(
            0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
    else
    {
        throw std::runtime_error("Invalid unicode code point in JSON string.");
    }
}
}

JsonParser::JsonParser(const std::string& text)
    : m_text(text)
{
}

JsonValue JsonParser::Parse()
{
    JsonValue value = ParseValue(0);
    SkipWhitespace();
    if (m_position != m_text.size())
    {
        throw std::runtime_error("Unexpected trailing characters in JSON.");
    }
    return value;
}

JsonValue JsonParser::ParseValue(std::size_t containerDepth)
{
    SkipWhitespace();
    if (m_position >= m_text.size())
    {
        throw std::runtime_error("Unexpected end of JSON.");
    }

    switch (m_text[m_position])
    {
    case '{':
        if (containerDepth >= MaximumJsonContainerDepth)
        {
            throw std::runtime_error("JSON nesting depth exceeds the limit.");
        }
        return ParseObject(containerDepth + 1);
    case '[':
        if (containerDepth >= MaximumJsonContainerDepth)
        {
            throw std::runtime_error("JSON nesting depth exceeds the limit.");
        }
        return ParseArray(containerDepth + 1);
    case '"':
    {
        JsonValue value;
        value.type = JsonValue::Type::String;
        value.string = ParseString();
        return value;
    }
    case 't': return ParseLiteral("true", JsonValue::Type::Bool, true);
    case 'f': return ParseLiteral("false", JsonValue::Type::Bool, false);
    case 'n': return ParseLiteral("null", JsonValue::Type::Null, false);
    default:
        if (m_text[m_position] == '-' ||
            (m_text[m_position] >= '0' && m_text[m_position] <= '9'))
        {
            return ParseNumber();
        }
        throw std::runtime_error("Invalid token in JSON.");
    }
}

JsonValue JsonParser::ParseObject(std::size_t containerDepth)
{
    JsonValue value;
    value.type = JsonValue::Type::Object;
    Expect('{');
    SkipWhitespace();
    if (TryConsume('}'))
    {
        return value;
    }

    while (true)
    {
        SkipWhitespace();
        const std::string key = ParseString();
        SkipWhitespace();
        Expect(':');
        value.object[key] = ParseValue(containerDepth);
        SkipWhitespace();
        if (TryConsume('}'))
        {
            break;
        }
        Expect(',');
    }
    return value;
}

JsonValue JsonParser::ParseArray(std::size_t containerDepth)
{
    JsonValue value;
    value.type = JsonValue::Type::Array;
    Expect('[');
    SkipWhitespace();
    if (TryConsume(']'))
    {
        return value;
    }

    while (true)
    {
        value.array.push_back(ParseValue(containerDepth));
        SkipWhitespace();
        if (TryConsume(']'))
        {
            break;
        }
        Expect(',');
    }
    return value;
}

JsonValue JsonParser::ParseLiteral(const char* literal, JsonValue::Type type, bool boolean)
{
    const std::size_t length = std::strlen(literal);
    if (m_text.compare(m_position, length, literal) != 0)
    {
        throw std::runtime_error("Invalid literal in JSON.");
    }
    m_position += length;

    JsonValue value;
    value.type = type;
    value.boolean = boolean;
    return value;
}

JsonValue JsonParser::ParseNumber()
{
    const std::size_t begin = m_position;
    if (m_text[m_position] == '-')
    {
        ++m_position;
    }
    if (m_position >= m_text.size())
    {
        throw std::runtime_error("Invalid number in JSON.");
    }
    if (m_text[m_position] == '0')
    {
        ++m_position;
        if (m_position < m_text.size() &&
            m_text[m_position] >= '0' && m_text[m_position] <= '9')
        {
            throw std::runtime_error("Invalid number in JSON.");
        }
    }
    else
    {
        if (m_text[m_position] < '1' || m_text[m_position] > '9')
        {
            throw std::runtime_error("Invalid number in JSON.");
        }
        ConsumeDigits();
    }
    if (m_position < m_text.size() && m_text[m_position] == '.')
    {
        ++m_position;
        ConsumeDigits();
    }
    if (m_position < m_text.size() && (m_text[m_position] == 'e' || m_text[m_position] == 'E'))
    {
        ++m_position;
        if (m_position < m_text.size() && (m_text[m_position] == '+' || m_text[m_position] == '-'))
        {
            ++m_position;
        }
        ConsumeDigits();
    }

    JsonValue value;
    value.type = JsonValue::Type::Number;
    const char* first = m_text.data() + begin;
    const char* last = m_text.data() + m_position;
    const std::from_chars_result converted = std::from_chars(
        first, last, value.number, std::chars_format::general);
    if (converted.ec != std::errc() || converted.ptr != last ||
        !std::isfinite(value.number))
    {
        throw std::runtime_error("Invalid number in JSON.");
    }
    return value;
}

std::string JsonParser::ParseString()
{
    Expect('"');
    std::string result;
    while (m_position < m_text.size())
    {
        const auto ch = static_cast<unsigned char>(m_text[m_position++]);
        if (ch == '"')
        {
            return result;
        }
        if (ch < 0x20u)
        {
            throw std::runtime_error(
                "Unescaped control character in JSON string.");
        }
        if (ch != '\\')
        {
            if (ch < 0x80u)
            {
                result.push_back(static_cast<char>(ch));
                continue;
            }

            const std::size_t sequenceBegin = m_position - 1;
            std::uint32_t codePoint = 0;
            std::uint32_t minimumCodePoint = 0;
            std::size_t continuationBytes = 0;
            if (ch >= 0xc2u && ch <= 0xdfu)
            {
                codePoint = ch & 0x1fu;
                minimumCodePoint = 0x80u;
                continuationBytes = 1;
            }
            else if (ch >= 0xe0u && ch <= 0xefu)
            {
                codePoint = ch & 0x0fu;
                minimumCodePoint = 0x800u;
                continuationBytes = 2;
            }
            else if (ch >= 0xf0u && ch <= 0xf4u)
            {
                codePoint = ch & 0x07u;
                minimumCodePoint = 0x10000u;
                continuationBytes = 3;
            }
            else
            {
                throw std::runtime_error("Invalid UTF-8 in JSON string.");
            }
            if (m_position > m_text.size() ||
                m_text.size() - m_position < continuationBytes)
            {
                throw std::runtime_error("Invalid UTF-8 in JSON string.");
            }
            for (std::size_t index = 0; index < continuationBytes; ++index)
            {
                const auto continuation =
                    static_cast<unsigned char>(m_text[m_position++]);
                if ((continuation & 0xc0u) != 0x80u)
                {
                    throw std::runtime_error("Invalid UTF-8 in JSON string.");
                }
                codePoint = (codePoint << 6u) | (continuation & 0x3fu);
            }
            if (codePoint < minimumCodePoint || codePoint > 0x10ffffu ||
                (codePoint >= 0xd800u && codePoint <= 0xdfffu))
            {
                throw std::runtime_error("Invalid UTF-8 in JSON string.");
            }
            result.append(
                m_text, sequenceBegin, continuationBytes + 1);
            continue;
        }

        if (m_position >= m_text.size())
        {
            throw std::runtime_error("Unterminated escape in JSON string.");
        }

        const char escaped = m_text[m_position++];
        switch (escaped)
        {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u':
        {
            std::uint32_t codePoint =
                ParseUnicodeCodeUnit(m_text, m_position);
            if (codePoint >= 0xd800u && codePoint <= 0xdbffu)
            {
                if (m_position > m_text.size() ||
                    m_text.size() - m_position < 2 ||
                    m_text[m_position] != '\\' ||
                    m_text[m_position + 1] != 'u')
                {
                    throw std::runtime_error(
                        "Invalid unicode surrogate pair in JSON string.");
                }
                m_position += 2;
                const std::uint32_t lowSurrogate =
                    ParseUnicodeCodeUnit(m_text, m_position);
                if (lowSurrogate < 0xdc00u || lowSurrogate > 0xdfffu)
                {
                    throw std::runtime_error(
                        "Invalid unicode surrogate pair in JSON string.");
                }
                codePoint = 0x10000u +
                    ((codePoint - 0xd800u) << 10u) +
                    (lowSurrogate - 0xdc00u);
            }
            else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu)
            {
                throw std::runtime_error(
                    "Invalid unicode surrogate in JSON string.");
            }
            AppendUtf8(codePoint, result);
            break;
        }
        default:
            throw std::runtime_error("Invalid escape in JSON string.");
        }
    }
    throw std::runtime_error("Unterminated string in JSON.");
}

void JsonParser::ConsumeDigits()
{
    bool consumed = false;
    while (m_position < m_text.size() &&
        m_text[m_position] >= '0' && m_text[m_position] <= '9')
    {
        consumed = true;
        ++m_position;
    }
    if (!consumed)
    {
        throw std::runtime_error("Invalid number in JSON.");
    }
}

void JsonParser::SkipWhitespace()
{
    while (m_position < m_text.size())
    {
        const char character = m_text[m_position];
        if (character != ' ' && character != '\t' &&
            character != '\r' && character != '\n')
        {
            break;
        }
        ++m_position;
    }
}

void JsonParser::Expect(char expected)
{
    if (m_position >= m_text.size() || m_text[m_position] != expected)
    {
        throw std::runtime_error("Unexpected character in JSON.");
    }
    ++m_position;
}

bool JsonParser::TryConsume(char expected)
{
    if (m_position < m_text.size() && m_text[m_position] == expected)
    {
        ++m_position;
        return true;
    }
    return false;
}

const JsonValue* FindMember(const JsonValue& value, const char* name)
{
    if (value.type != JsonValue::Type::Object)
    {
        return nullptr;
    }
    const auto it = value.object.find(name);
    return it != value.object.end() ? &it->second : nullptr;
}

std::string JsonStringOr(const JsonValue& value, const char* name, const std::string& fallback)
{
    const JsonValue* member = FindMember(value, name);
    return member && member->type == JsonValue::Type::String ? member->string : fallback;
}

double JsonNumberOr(const JsonValue& value, const char* name, double fallback)
{
    const JsonValue* member = FindMember(value, name);
    return member && member->type == JsonValue::Type::Number ? member->number : fallback;
}

bool JsonBoolOr(const JsonValue& value, const char* name, bool fallback)
{
    const JsonValue* member = FindMember(value, name);
    return member && member->type == JsonValue::Type::Bool ? member->boolean : fallback;
}

std::array<float, 3> JsonFloat3Or(const JsonValue& value, const char* name, const std::array<float, 3>& fallback)
{
    const JsonValue* member = FindMember(value, name);
    if (!member || member->type != JsonValue::Type::Array || member->array.size() < 3)
    {
        return fallback;
    }

    std::array<float, 3> result = fallback;
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        if (member->array[i].type != JsonValue::Type::Number)
        {
            return fallback;
        }
        result[i] = static_cast<float>(member->array[i].number);
    }
    return result;
}

std::array<float, 4> JsonFloat4Or(const JsonValue& value, const char* name, const std::array<float, 4>& fallback)
{
    const JsonValue* member = FindMember(value, name);
    if (!member || member->type != JsonValue::Type::Array || member->array.size() < 4)
    {
        return fallback;
    }

    std::array<float, 4> result = fallback;
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        if (member->array[i].type != JsonValue::Type::Number)
        {
            return fallback;
        }
        result[i] = static_cast<float>(member->array[i].number);
    }
    return result;
}

std::string EscapeJson(const std::string& text)
{
    std::ostringstream escaped;
    for (const unsigned char ch : text)
    {
        switch (ch)
        {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\b': escaped << "\\b"; break;
        case '\f': escaped << "\\f"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (ch < 0x20)
            {
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
            }
            else
            {
                escaped << static_cast<char>(ch);
            }
            break;
        }
    }
    return escaped.str();
}

std::string JsonValueToJson(const JsonValue& value)
{
    std::ostringstream json;
    switch (value.type)
    {
    case JsonValue::Type::Null:
        json << "null";
        break;
    case JsonValue::Type::Bool:
        json << (value.boolean ? "true" : "false");
        break;
    case JsonValue::Type::Number:
        json << value.number;
        break;
    case JsonValue::Type::String:
        json << "\"" << EscapeJson(value.string) << "\"";
        break;
    case JsonValue::Type::Array:
        json << "[";
        for (std::size_t i = 0; i < value.array.size(); ++i)
        {
            if (i > 0)
            {
                json << ",";
            }
            json << JsonValueToJson(value.array[i]);
        }
        json << "]";
        break;
    case JsonValue::Type::Object:
    {
        json << "{";
        std::size_t index = 0;
        for (const auto& [key, member] : value.object)
        {
            if (index++ > 0)
            {
                json << ",";
            }
            json << "\"" << EscapeJson(key) << "\":" << JsonValueToJson(member);
        }
        json << "}";
        break;
    }
    }
    return json.str();
}

std::string TrimAscii(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.pop_back();
    }
    return text;
}
}
