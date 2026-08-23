#include "pch.h"
#include "AssistantMarkdownRenderer.h"

#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Text.h>

#include <cwctype>
#include <limits>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Documents;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace
{
constexpr unsigned MaximumInlineNesting = 12;
constexpr std::size_t MaximumLinkCharacters = 2048;

std::wstring NormalizeLineEndings(std::wstring_view value)
{
    std::wstring result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        if (value[index] == L'\r')
        {
            if (index + 1 < value.size() && value[index + 1] == L'\n')
            {
                ++index;
            }
            result.push_back(L'\n');
        }
        else
        {
            result.push_back(value[index]);
        }
    }
    return result;
}

std::vector<std::wstring_view> SplitLines(std::wstring const& value)
{
    std::vector<std::wstring_view> lines;
    std::size_t begin = 0;
    while (begin <= value.size())
    {
        const std::size_t end = value.find(L'\n', begin);
        if (end == std::wstring::npos)
        {
            lines.emplace_back(value.data() + begin, value.size() - begin);
            break;
        }
        lines.emplace_back(value.data() + begin, end - begin);
        begin = end + 1;
    }
    return lines;
}

std::wstring_view TrimView(std::wstring_view value)
{
    while (!value.empty() && std::iswspace(value.front()))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::iswspace(value.back()))
    {
        value.remove_suffix(1);
    }
    return value;
}

bool IsBlank(std::wstring_view value)
{
    return TrimView(value).empty();
}

bool IsEscaped(std::wstring_view value, std::size_t position)
{
    std::size_t slashCount = 0;
    while (position > slashCount && value[position - slashCount - 1] == L'\\')
    {
        ++slashCount;
    }
    return (slashCount % 2) != 0;
}

std::size_t FindUnescaped(
    std::wstring_view value,
    std::wstring_view marker,
    std::size_t begin)
{
    std::size_t found = value.find(marker, begin);
    while (found != std::wstring_view::npos && IsEscaped(value, found))
    {
        found = value.find(marker, found + marker.size());
    }
    return found;
}

void AppendRun(InlineCollection const& inlines, std::wstring_view text)
{
    if (text.empty())
    {
        return;
    }
    Run run;
    run.Text(hstring(text));
    inlines.Append(run);
}

bool EqualsAsciiInsensitive(
    std::wstring_view left,
    std::wstring_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (std::towlower(left[index]) != std::towlower(right[index]))
        {
            return false;
        }
    }
    return true;
}

std::optional<Uri> SafeLinkUri(std::wstring_view rawTarget)
{
    std::wstring_view target = TrimView(rawTarget);
    const std::size_t titleSeparator = target.find_first_of(L" \t");
    if (titleSeparator != std::wstring_view::npos)
    {
        target = target.substr(0, titleSeparator);
    }
    if (target.size() > 1 && target.front() == L'<' && target.back() == L'>')
    {
        target.remove_prefix(1);
        target.remove_suffix(1);
    }
    if (target.empty() || target.size() > MaximumLinkCharacters)
    {
        return std::nullopt;
    }
    for (const wchar_t character : target)
    {
        if (character < 0x20 || character == 0x7f)
        {
            return std::nullopt;
        }
    }

    const std::size_t colon = target.find(L':');
    if (colon == std::wstring_view::npos)
    {
        return std::nullopt;
    }
    const std::wstring_view scheme = target.substr(0, colon);
    if (!EqualsAsciiInsensitive(scheme, L"https") &&
        !EqualsAsciiInsensitive(scheme, L"http") &&
        !EqualsAsciiInsensitive(scheme, L"mailto"))
    {
        return std::nullopt;
    }

    try
    {
        return Uri(hstring(target));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

void AppendInlineMarkdown(
    InlineCollection const& inlines,
    std::wstring_view text,
    unsigned depth);

void AppendStyledSpan(
    InlineCollection const& inlines,
    std::wstring_view text,
    unsigned depth,
    bool bold,
    bool italic,
    bool strike)
{
    Span span;
    if (bold)
    {
        span.FontWeight(FontWeights::SemiBold());
    }
    if (italic)
    {
        span.FontStyle(FontStyle::Italic);
    }
    if (strike)
    {
        span.TextDecorations(TextDecorations::Strikethrough);
    }
    AppendInlineMarkdown(span.Inlines(), text, depth + 1);
    inlines.Append(span);
}

void AppendLink(
    InlineCollection const& inlines,
    std::wstring_view label,
    std::wstring_view target,
    unsigned depth)
{
    const std::optional<Uri> uri = SafeLinkUri(target);
    if (!uri)
    {
        AppendInlineMarkdown(inlines, label, depth + 1);
        return;
    }

    Hyperlink link;
    link.NavigateUri(*uri);
    AppendInlineMarkdown(link.Inlines(), label, depth + 1);
    inlines.Append(link);
}

void AppendInlineMarkdown(
    InlineCollection const& inlines,
    std::wstring_view text,
    unsigned depth)
{
    if (depth >= MaximumInlineNesting)
    {
        AppendRun(inlines, text);
        return;
    }

    std::wstring literal;
    const auto flushLiteral = [&]()
    {
        AppendRun(inlines, literal);
        literal.clear();
    };

    std::size_t index = 0;
    while (index < text.size())
    {
        if (text[index] == L'\n')
        {
            flushLiteral();
            inlines.Append(LineBreak());
            ++index;
            continue;
        }

        if (text[index] == L'\\' && index + 1 < text.size() &&
            std::wstring_view(L"\\`*_{}[]()#+-.!|>~").find(text[index + 1]) !=
                std::wstring_view::npos)
        {
            literal.push_back(text[index + 1]);
            index += 2;
            continue;
        }

        if (text[index] == L'`')
        {
            std::size_t markerLength = 1;
            while (index + markerLength < text.size() &&
                   text[index + markerLength] == L'`')
            {
                ++markerLength;
            }
            const std::wstring_view marker = text.substr(index, markerLength);
            const std::size_t close = FindUnescaped(
                text, marker, index + markerLength);
            if (close != std::wstring_view::npos)
            {
                flushLiteral();
                Run code;
                code.Text(hstring(text.substr(
                    index + markerLength,
                    close - index - markerLength)));
                code.FontFamily(FontFamily(L"Consolas"));
                inlines.Append(code);
                index = close + markerLength;
                continue;
            }
        }

        if (text[index] == L'!' && index + 1 < text.size() &&
            text[index + 1] == L'[')
        {
            const std::size_t labelEnd = FindUnescaped(text, L"](", index + 2);
            const std::size_t targetEnd = labelEnd == std::wstring_view::npos
                ? std::wstring_view::npos
                : FindUnescaped(text, L")", labelEnd + 2);
            if (targetEnd != std::wstring_view::npos)
            {
                flushLiteral();
                const std::wstring_view label = text.substr(
                    index + 2, labelEnd - index - 2);
                AppendRun(inlines, L"Image: ");
                AppendLink(
                    inlines,
                    label.empty() ? L"image" : label,
                    text.substr(labelEnd + 2, targetEnd - labelEnd - 2),
                    depth);
                index = targetEnd + 1;
                continue;
            }
        }

        if (text[index] == L'[')
        {
            const std::size_t labelEnd = FindUnescaped(text, L"](", index + 1);
            const std::size_t targetEnd = labelEnd == std::wstring_view::npos
                ? std::wstring_view::npos
                : FindUnescaped(text, L")", labelEnd + 2);
            if (targetEnd != std::wstring_view::npos)
            {
                flushLiteral();
                AppendLink(
                    inlines,
                    text.substr(index + 1, labelEnd - index - 1),
                    text.substr(labelEnd + 2, targetEnd - labelEnd - 2),
                    depth);
                index = targetEnd + 1;
                continue;
            }
        }

        if (text[index] == L'<' )
        {
            const std::size_t close = text.find(L'>', index + 1);
            if (close != std::wstring_view::npos)
            {
                const std::wstring_view target = text.substr(
                    index + 1, close - index - 1);
                if (SafeLinkUri(target))
                {
                    flushLiteral();
                    AppendLink(inlines, target, target, depth);
                    index = close + 1;
                    continue;
                }
            }
        }

        struct Delimiter
        {
            std::wstring_view marker;
            bool bold;
            bool italic;
            bool strike;
        };
        constexpr std::array<Delimiter, 5> delimiters = {{
            { L"**", true, false, false },
            { L"__", true, false, false },
            { L"~~", false, false, true },
            { L"*", false, true, false },
            { L"_", false, true, false },
        }};
        bool matchedDelimiter = false;
        for (const Delimiter& delimiter : delimiters)
        {
            if (!text.substr(index).starts_with(delimiter.marker))
            {
                continue;
            }
            const std::size_t contentBegin =
                index + delimiter.marker.size();
            if (contentBegin >= text.size() ||
                std::iswspace(text[contentBegin]))
            {
                continue;
            }
            if (delimiter.marker.front() == L'_' && index > 0 &&
                std::iswalnum(text[index - 1]) &&
                std::iswalnum(text[contentBegin]))
            {
                continue;
            }

            std::size_t close = FindUnescaped(
                text, delimiter.marker, contentBegin);
            while (close != std::wstring_view::npos)
            {
                const std::size_t afterClose =
                    close + delimiter.marker.size();
                const bool closesAfterText = close > contentBegin &&
                    !std::iswspace(text[close - 1]);
                const bool underscoreInsideWord =
                    delimiter.marker.front() == L'_' &&
                    afterClose < text.size() &&
                    std::iswalnum(text[close - 1]) &&
                    std::iswalnum(text[afterClose]);
                if (closesAfterText && !underscoreInsideWord)
                {
                    break;
                }
                close = FindUnescaped(
                    text,
                    delimiter.marker,
                    close + delimiter.marker.size());
            }
            if (close == std::wstring_view::npos ||
                close == index + delimiter.marker.size())
            {
                continue;
            }
            flushLiteral();
            AppendStyledSpan(
                inlines,
                text.substr(
                    index + delimiter.marker.size(),
                    close - index - delimiter.marker.size()),
                depth,
                delimiter.bold,
                delimiter.italic,
                delimiter.strike);
            index = close + delimiter.marker.size();
            matchedDelimiter = true;
            break;
        }
        if (matchedDelimiter)
        {
            continue;
        }

        literal.push_back(text[index]);
        ++index;
    }
    flushLiteral();
}

Paragraph NewParagraph(Thickness const& margin = Thickness{ 0, 0, 0, 7 })
{
    Paragraph paragraph;
    paragraph.Margin(margin);
    return paragraph;
}

void AppendParagraph(
    BlockCollection const& blocks,
    std::wstring_view text,
    Thickness const& margin = Thickness{ 0, 0, 0, 7 })
{
    Paragraph paragraph = NewParagraph(margin);
    AppendInlineMarkdown(paragraph.Inlines(), text, 0);
    blocks.Append(paragraph);
}

struct Fence
{
    wchar_t character = L'\0';
    std::size_t length = 0;
};

std::optional<Fence> ParseFence(std::wstring_view line)
{
    std::size_t indentation = 0;
    while (indentation < line.size() && indentation < 4 &&
           line[indentation] == L' ')
    {
        ++indentation;
    }
    if (indentation > 3 || indentation >= line.size() ||
        (line[indentation] != L'`' && line[indentation] != L'~'))
    {
        return std::nullopt;
    }
    const wchar_t marker = line[indentation];
    std::size_t length = 0;
    while (indentation + length < line.size() &&
           line[indentation + length] == marker)
    {
        ++length;
    }
    if (length < 3)
    {
        return std::nullopt;
    }
    return Fence{ marker, length };
}

bool IsClosingFence(std::wstring_view line, Fence const& fence)
{
    const std::optional<Fence> candidate = ParseFence(line);
    return candidate && candidate->character == fence.character &&
           candidate->length >= fence.length;
}

std::optional<unsigned> HeadingLevel(std::wstring_view line)
{
    std::size_t indentation = 0;
    while (indentation < line.size() && indentation < 4 &&
           line[indentation] == L' ')
    {
        ++indentation;
    }
    if (indentation > 3)
    {
        return std::nullopt;
    }
    unsigned level = 0;
    while (indentation + level < line.size() && level < 6 &&
           line[indentation + level] == L'#')
    {
        ++level;
    }
    if (level == 0 || indentation + level >= line.size() ||
        line[indentation + level] != L' ')
    {
        return std::nullopt;
    }
    return level;
}

bool IsHorizontalRule(std::wstring_view line)
{
    line = TrimView(line);
    if (line.size() < 3 ||
        (line.front() != L'-' && line.front() != L'*' &&
         line.front() != L'_'))
    {
        return false;
    }
    const wchar_t marker = line.front();
    std::size_t count = 0;
    for (const wchar_t character : line)
    {
        if (character == marker)
        {
            ++count;
        }
        else if (character != L' ' && character != L'\t')
        {
            return false;
        }
    }
    return count >= 3;
}

struct ListItem
{
    std::size_t indentation = 0;
    std::wstring_view marker;
    std::wstring_view content;
};

std::optional<ListItem> ParseListItem(std::wstring_view line)
{
    std::size_t indentation = 0;
    while (indentation < line.size() && line[indentation] == L' ')
    {
        ++indentation;
    }
    if (indentation >= line.size())
    {
        return std::nullopt;
    }

    if ((line[indentation] == L'-' || line[indentation] == L'+' ||
         line[indentation] == L'*') &&
        indentation + 1 < line.size() && line[indentation + 1] == L' ')
    {
        return ListItem{
            indentation,
            line.substr(indentation, 1),
            line.substr(indentation + 2) };
    }

    std::size_t digitsEnd = indentation;
    while (digitsEnd < line.size() && std::iswdigit(line[digitsEnd]))
    {
        ++digitsEnd;
    }
    if (digitsEnd > indentation && digitsEnd + 1 < line.size() &&
        (line[digitsEnd] == L'.' || line[digitsEnd] == L')') &&
        line[digitsEnd + 1] == L' ')
    {
        return ListItem{
            indentation,
            line.substr(indentation, digitsEnd - indentation + 1),
            line.substr(digitsEnd + 2) };
    }
    return std::nullopt;
}

bool StartsBlock(std::vector<std::wstring_view> const& lines, std::size_t index)
{
    if (index >= lines.size() || IsBlank(lines[index]))
    {
        return true;
    }
    const std::wstring_view trimmed = TrimView(lines[index]);
    return ParseFence(lines[index]).has_value() ||
           HeadingLevel(lines[index]).has_value() ||
           ParseListItem(lines[index]).has_value() ||
           IsHorizontalRule(lines[index]) ||
           trimmed.starts_with(L">");
}

std::wstring JoinParagraphLines(
    std::vector<std::wstring_view> const& lines,
    std::size_t begin,
    std::size_t end)
{
    std::wstring paragraph;
    for (std::size_t index = begin; index < end; ++index)
    {
        std::wstring_view line = lines[index];
        while (!line.empty() && std::iswspace(line.front()))
        {
            line.remove_prefix(1);
        }
        bool hardBreak = false;
        if (line.ends_with(L"\\"))
        {
            line.remove_suffix(1);
            hardBreak = true;
        }
        else if (line.size() >= 2 && line.ends_with(L"  "))
        {
            line.remove_suffix(2);
            hardBreak = true;
        }
        while (!line.empty() && std::iswspace(line.back()))
        {
            line.remove_suffix(1);
        }
        paragraph.append(line);
        if (index + 1 < end)
        {
            paragraph.push_back(hardBreak ? L'\n' : L' ');
        }
    }
    return paragraph;
}

void RenderMarkdownBlocks(
    RichTextBlock const& target,
    std::wstring const& normalized)
{
    const std::vector<std::wstring_view> lines = SplitLines(normalized);
    BlockCollection blocks = target.Blocks();
    blocks.Clear();

    std::size_t index = 0;
    while (index < lines.size())
    {
        if (IsBlank(lines[index]))
        {
            ++index;
            continue;
        }

        if (const std::optional<Fence> fence = ParseFence(lines[index]))
        {
            ++index;
            std::wstring code;
            while (index < lines.size() && !IsClosingFence(lines[index], *fence))
            {
                if (!code.empty())
                {
                    code.push_back(L'\n');
                }
                code.append(lines[index]);
                ++index;
            }
            if (index < lines.size())
            {
                ++index;
            }
            Paragraph paragraph = NewParagraph(Thickness{ 8, 4, 0, 9 });
            paragraph.FontFamily(FontFamily(L"Consolas"));
            paragraph.FontSize(12.0);
            AppendRun(paragraph.Inlines(), code);
            blocks.Append(paragraph);
            continue;
        }

        if (const std::optional<unsigned> level = HeadingLevel(lines[index]))
        {
            std::wstring_view line = TrimView(lines[index]);
            line.remove_prefix(*level);
            line = TrimView(line);
            while (!line.empty() && line.back() == L'#')
            {
                line.remove_suffix(1);
            }
            line = TrimView(line);
            static constexpr std::array<double, 6> sizes = {
                22.0, 20.0, 18.0, 16.0, 15.0, 14.0 };
            Paragraph paragraph = NewParagraph(Thickness{ 0, 6, 0, 5 });
            paragraph.FontSize(sizes[*level - 1]);
            paragraph.FontWeight(FontWeights::SemiBold());
            AppendInlineMarkdown(paragraph.Inlines(), line, 0);
            blocks.Append(paragraph);
            ++index;
            continue;
        }

        if (IsHorizontalRule(lines[index]))
        {
            Paragraph paragraph = NewParagraph(Thickness{ 0, 2, 0, 7 });
            AppendRun(paragraph.Inlines(), L"────────────────────────");
            blocks.Append(paragraph);
            ++index;
            continue;
        }

        if (TrimView(lines[index]).starts_with(L">"))
        {
            std::wstring quote;
            while (index < lines.size())
            {
                std::wstring_view line = TrimView(lines[index]);
                if (!line.starts_with(L">"))
                {
                    break;
                }
                line.remove_prefix(1);
                line = TrimView(line);
                if (!quote.empty())
                {
                    quote.push_back(L'\n');
                }
                quote.append(line);
                ++index;
            }
            Paragraph paragraph = NewParagraph(Thickness{ 12, 2, 0, 8 });
            paragraph.FontStyle(FontStyle::Italic);
            AppendRun(paragraph.Inlines(), L"│ ");
            AppendInlineMarkdown(paragraph.Inlines(), quote, 0);
            blocks.Append(paragraph);
            continue;
        }

        if (const std::optional<ListItem> item = ParseListItem(lines[index]))
        {
            std::wstring_view content = item->content;
            std::wstring prefix;
            if (content.size() >= 4 && content.front() == L'[' &&
                content[2] == L']' && content[3] == L' ' &&
                (content[1] == L' ' || content[1] == L'x' ||
                 content[1] == L'X'))
            {
                prefix = content[1] == L' ' ? L"☐ " : L"☑ ";
                content.remove_prefix(4);
            }
            else if (item->marker == L"-" || item->marker == L"+" ||
                     item->marker == L"*")
            {
                prefix = L"• ";
            }
            else
            {
                prefix.assign(item->marker);
                prefix.push_back(L' ');
            }
            Paragraph paragraph = NewParagraph(Thickness{
                14.0 + static_cast<double>(item->indentation) * 7.0,
                0,
                0,
                3 });
            AppendRun(paragraph.Inlines(), prefix);
            AppendInlineMarkdown(paragraph.Inlines(), content, 0);
            blocks.Append(paragraph);
            ++index;
            continue;
        }

        const std::size_t paragraphBegin = index;
        ++index;
        while (index < lines.size() && !StartsBlock(lines, index))
        {
            ++index;
        }
        AppendParagraph(
            blocks,
            JoinParagraphLines(lines, paragraphBegin, index));
    }

    if (blocks.Size() == 0)
    {
        blocks.Append(NewParagraph(Thickness{}));
    }
}
}

namespace lookdevpt::ui
{
void RenderAssistantMarkdown(
    RichTextBlock const& target,
    std::wstring_view markdown)
{
    RenderMarkdownBlocks(target, NormalizeLineEndings(markdown));
}

void RenderAssistantPlainText(
    RichTextBlock const& target,
    std::wstring_view text)
{
    BlockCollection blocks = target.Blocks();
    blocks.Clear();
    Paragraph paragraph = NewParagraph(Thickness{});
    const std::wstring normalized = NormalizeLineEndings(text);
    std::size_t begin = 0;
    while (begin <= normalized.size())
    {
        const std::size_t end = normalized.find(L'\n', begin);
        if (end == std::wstring::npos)
        {
            AppendRun(
                paragraph.Inlines(),
                std::wstring_view(normalized).substr(begin));
            break;
        }
        AppendRun(
            paragraph.Inlines(),
            std::wstring_view(normalized).substr(begin, end - begin));
        paragraph.Inlines().Append(LineBreak());
        begin = end + 1;
    }
    blocks.Append(paragraph);
}
}
