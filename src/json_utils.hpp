#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

class JsonObjectParser
{
private:
    const std::string& input;
    std::size_t position = 0;
    std::string error;

    void skipWhitespace()
    {
        while (position < input.size()
            && (input[position] == ' ' || input[position] == '\t'
                || input[position] == '\r' || input[position] == '\n'))
        {
            ++position;
        }
    }

    bool consume(char expected)
    {
        skipWhitespace();
        if (position == input.size() || input[position] != expected)
        {
            return false;
        }
        ++position;
        return true;
    }

    bool appendCodePoint(std::string& output, std::uint32_t codePoint)
    {
        if (codePoint <= 0x7f)
        {
            output.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint <= 0x7ff)
        {
            output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
        else if (codePoint <= 0xffff)
        {
            output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
        else if (codePoint <= 0x10ffff)
        {
            output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
        else
        {
            error = "Unicode escape is outside the valid range";
            return false;
        }
        return true;
    }

    bool readHexQuad(std::uint32_t& value)
    {
        if (input.size() - position < 4)
        {
            error = "Incomplete Unicode escape";
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char ch = input[position++];
            value <<= 4;
            if (ch >= '0' && ch <= '9')
            {
                value |= static_cast<std::uint32_t>(ch - '0');
            }
            else if (ch >= 'a' && ch <= 'f')
            {
                value |= static_cast<std::uint32_t>(ch - 'a' + 10);
            }
            else if (ch >= 'A' && ch <= 'F')
            {
                value |= static_cast<std::uint32_t>(ch - 'A' + 10);
            }
            else
            {
                error = "Invalid Unicode escape";
                return false;
            }
        }
        return true;
    }

    bool parseString(std::string& value)
    {
        skipWhitespace();
        if (position == input.size() || input[position++] != '"')
        {
            error = "Expected a JSON string";
            return false;
        }

        value.clear();
        while (position < input.size())
        {
            const unsigned char ch = static_cast<unsigned char>(input[position++]);
            if (ch == '"')
            {
                return true;
            }
            if (ch < 0x20)
            {
                error = "Unescaped control character in JSON string";
                return false;
            }
            if (ch != '\\')
            {
                value.push_back(static_cast<char>(ch));
                continue;
            }
            if (position == input.size())
            {
                error = "Incomplete JSON escape";
                return false;
            }

            const char escaped = input[position++];
            switch (escaped)
            {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u':
            {
                std::uint32_t codePoint = 0;
                if (!readHexQuad(codePoint))
                {
                    return false;
                }
                if (codePoint >= 0xd800 && codePoint <= 0xdbff)
                {
                    if (input.size() - position < 6
                        || input[position] != '\\' || input[position + 1] != 'u')
                    {
                        error = "High surrogate is missing its low surrogate";
                        return false;
                    }
                    position += 2;
                    std::uint32_t lowSurrogate = 0;
                    if (!readHexQuad(lowSurrogate)
                        || lowSurrogate < 0xdc00 || lowSurrogate > 0xdfff)
                    {
                        error = "Invalid low surrogate";
                        return false;
                    }
                    codePoint = 0x10000
                        + ((codePoint - 0xd800) << 10)
                        + (lowSurrogate - 0xdc00);
                }
                else if (codePoint >= 0xdc00 && codePoint <= 0xdfff)
                {
                    error = "Unexpected low surrogate";
                    return false;
                }
                if (!appendCodePoint(value, codePoint))
                {
                    return false;
                }
                break;
            }
            default:
                error = "Invalid JSON escape";
                return false;
            }
        }

        error = "Unterminated JSON string";
        return false;
    }

    bool parseNumber()
    {
        const std::size_t start = position;
        if (position < input.size() && input[position] == '-')
        {
            ++position;
        }
        if (position == input.size())
        {
            error = "Invalid JSON number";
            return false;
        }

        if (input[position] == '0')
        {
            ++position;
            if (position < input.size() && input[position] >= '0' && input[position] <= '9')
            {
                error = "Leading zero in JSON number";
                return false;
            }
        }
        else if (input[position] >= '1' && input[position] <= '9')
        {
            while (position < input.size()
                && input[position] >= '0' && input[position] <= '9')
            {
                ++position;
            }
        }
        else
        {
            error = "Invalid JSON number";
            return false;
        }

        if (position < input.size() && input[position] == '.')
        {
            ++position;
            const std::size_t fractionStart = position;
            while (position < input.size()
                && input[position] >= '0' && input[position] <= '9')
            {
                ++position;
            }
            if (position == fractionStart)
            {
                error = "Invalid JSON fraction";
                return false;
            }
        }

        if (position < input.size() && (input[position] == 'e' || input[position] == 'E'))
        {
            ++position;
            if (position < input.size() && (input[position] == '+' || input[position] == '-'))
            {
                ++position;
            }
            const std::size_t exponentStart = position;
            while (position < input.size()
                && input[position] >= '0' && input[position] <= '9')
            {
                ++position;
            }
            if (position == exponentStart)
            {
                error = "Invalid JSON exponent";
                return false;
            }
        }

        return position > start;
    }

    bool parseLiteral(const char* literal)
    {
        std::size_t i = 0;
        while (literal[i] != '\0')
        {
            if (position == input.size() || input[position] != literal[i])
            {
                error = "Invalid JSON value";
                return false;
            }
            ++position;
            ++i;
        }
        return true;
    }

    bool parseValue(int depth)
    {
        if (depth > 64)
        {
            error = "JSON nesting limit exceeded";
            return false;
        }
        skipWhitespace();
        if (position == input.size())
        {
            error = "Expected a JSON value";
            return false;
        }

        switch (input[position])
        {
        case '"':
        {
            std::string ignored;
            return parseString(ignored);
        }
        case '{':
            ++position;
            skipWhitespace();
            if (consume('}'))
            {
                return true;
            }
            while (true)
            {
                std::string key;
                if (!parseString(key) || !consume(':') || !parseValue(depth + 1))
                {
                    return false;
                }
                if (consume('}'))
                {
                    return true;
                }
                if (!consume(','))
                {
                    error = "Expected ',' or '}' in JSON object";
                    return false;
                }
            }
        case '[':
            ++position;
            skipWhitespace();
            if (consume(']'))
            {
                return true;
            }
            while (true)
            {
                if (!parseValue(depth + 1))
                {
                    return false;
                }
                if (consume(']'))
                {
                    return true;
                }
                if (!consume(','))
                {
                    error = "Expected ',' or ']' in JSON array";
                    return false;
                }
            }
        case 't': return parseLiteral("true");
        case 'f': return parseLiteral("false");
        case 'n': return parseLiteral("null");
        default: return parseNumber();
        }
    }

public:
    explicit JsonObjectParser(const std::string& json)
        : input(json)
    {
    }

    bool parse(std::map<std::string, std::optional<std::string>>& fields,
               std::string& parseError)
    {
        fields.clear();
        if (!consume('{'))
        {
            error = "Top-level JSON value must be an object";
            parseError = error;
            return false;
        }
        skipWhitespace();
        if (!consume('}'))
        {
            while (true)
            {
                std::string key;
                if (!parseString(key) || !consume(':'))
                {
                    if (error.empty())
                    {
                        error = "Expected a JSON object member";
                    }
                    parseError = error;
                    return false;
                }

                if (fields.find(key) != fields.end())
                {
                    error = "Duplicate JSON object member: " + key;
                    parseError = error;
                    return false;
                }

                skipWhitespace();
                if (position < input.size() && input[position] == '"')
                {
                    std::string value;
                    if (!parseString(value))
                    {
                        parseError = error;
                        return false;
                    }
                    fields.emplace(std::move(key), std::move(value));
                }
                else
                {
                    if (!parseValue(0))
                    {
                        parseError = error;
                        return false;
                    }
                    fields.emplace(std::move(key), std::nullopt);
                }

                if (consume('}'))
                {
                    break;
                }
                if (!consume(','))
                {
                    error = "Expected ',' or '}' in JSON object";
                    parseError = error;
                    return false;
                }
            }
        }

        skipWhitespace();
        if (position != input.size())
        {
            error = "Unexpected content after JSON object";
            parseError = error;
            return false;
        }
        return true;
    }
};

inline std::string jsonEscape(const std::string& value)
{
    static const char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                escaped += "\\u00";
                escaped.push_back(hex[ch >> 4]);
                escaped.push_back(hex[ch & 0x0f]);
            }
            else
            {
                escaped.push_back(static_cast<char>(ch));
            }
        }
    }
    return escaped;
}
