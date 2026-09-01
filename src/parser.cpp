#include "resonant/graph.hpp"

#include "decimal.hpp"
#include <cmath>
#include <utility>

namespace resonant {
Diagnostic::Diagnostic(SourceSpan span, std::string message)
    : std::runtime_error(std::to_string(span.line) + ":" + std::to_string(span.column) + ": " + message),
      span_(span) {}

namespace {
enum class Kind { end, word, number, assign, open, close, comma, semicolon };
struct Token {
    Kind kind;
    SourceSpan span;
    std::string_view text;
};

bool letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool digit(char c) {
    return c >= '0' && c <= '9';
}

class Lexer {
public:
    explicit Lexer(std::string_view text) : source_(text) {
        if (text.size() > max_source_bytes) throw Diagnostic({0, 0, 1, 1}, "source exceeds byte limit");
    }

    Token next() {
        while (offset_ < source_.size()) {
            const char c = source_[offset_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                consume();
            } else if (c == '#') {
                while (offset_ < source_.size() && source_[offset_] != '\n') consume();
            } else {
                break;
            }
        }
        const SourceSpan begin{offset_, 0, line_, column_};
        if (offset_ == source_.size()) return {Kind::end, begin, {}};
        const char c = consume();
        Kind kind;
        switch (c) {
        case '=': kind = Kind::assign; break;
        case '(': kind = Kind::open; break;
        case ')': kind = Kind::close; break;
        case ',': kind = Kind::comma; break;
        case ';': kind = Kind::semicolon; break;
        default:
            if (letter(c)) {
                kind = Kind::word;
                while (offset_ < source_.size() && (letter(source_[offset_]) || digit(source_[offset_]))) consume();
                if (offset_ - begin.offset > 63) throw Diagnostic(begin, "identifier exceeds 63 bytes");
            } else if (digit(c) || c == '.' || c == '+' || c == '-') {
                kind = Kind::number;
                while (offset_ < source_.size()) {
                    const char next = source_[offset_];
                    if (!digit(next) && next != 'e' && next != 'E' && next != '.' && next != '+' && next != '-') break;
                    consume();
                }
                if (offset_ - begin.offset > 63) throw Diagnostic(begin, "numeric token exceeds 63 bytes");
            } else {
                throw Diagnostic(begin, "unexpected source character");
            }
        }
        auto span = begin;
        span.length = offset_ - begin.offset;
        return {kind, span, source_.substr(span.offset, span.length)};
    }

private:
    char consume() {
        const char c = source_[offset_++];
        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return c;
    }

    std::string_view source_;
    std::uint32_t offset_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t column_ = 1;
};

float number(Token token) {
    auto text = token.text;
    if (text.starts_with('+')) text.remove_prefix(1);
    const auto value = decimal<float>(text);
    if (!value || std::abs(*value) > signal_limit) {
        throw Diagnostic(token.span, "expected a finite numeric literal within the signal range");
    }
    return *value;
}

class Parser {
public:
    explicit Parser(std::string_view source) : lexer_(source), token_(lexer_.next()) {}

    Graph run() {
        Graph graph;
        while (token_.kind != Kind::end) {
            const auto first = expect(Kind::word, "expected a declaration or out");
            if (first.text == "out") {
                if (!graph.outputs.empty()) throw Diagnostic(first.span, "duplicate output statement");
                graph.outputs = arguments(2);
                if (graph.outputs.empty()) throw Diagnostic(first.span, "out requires one or two signals");
                graph.output_span = complete(first.span);
                continue;
            }
            if (first.text == "impulse") {
                if (graph.impulses.size() >= max_states) throw Diagnostic(first.span, "too many impulse assets");
                graph.impulses.push_back(impulse(first.span));
                continue;
            }
            if (graph.expressions.size() >= max_declarations) throw Diagnostic(first.span, "too many declarations");
            const bool parameter = first.text == "param";
            const auto name = parameter ? expect(Kind::word, "expected parameter name") : first;
            if (name.text == "out" || name.text == "param" || name.text == "impulse") throw Diagnostic(name.span, "reserved declaration name");
            expect(Kind::assign, "expected '='");
            if (parameter || token_.kind == Kind::number) {
                const auto value = expect(Kind::number, "expected numeric default");
                auto span = complete(first.span);
                graph.expressions.push_back({parameter ? Declaration::parameter : Declaration::literal,
                                             std::string(name.text), {}, span, {{value.span, {}, number(value)}}});
            } else {
                const auto op = expect(Kind::word, "expected an operation name or number");
                auto args = arguments(3);
                auto span = complete(first.span);
                graph.expressions.push_back({Declaration::node, std::string(name.text), std::string(op.text),
                                             span, std::move(args)});
            }
        }
        if (graph.outputs.empty()) throw Diagnostic(token_.span, "missing out statement");
        return graph;
    }

private:
    Token expect(Kind kind, const char* message) {
        if (token_.kind != kind) throw Diagnostic(token_.span, message);
        const auto token = token_;
        token_ = lexer_.next();
        return token;
    }

    SourceSpan complete(SourceSpan begin) {
        const auto end = expect(Kind::semicolon, "expected ';'").span;
        begin.length = end.offset + end.length - begin.offset;
        return begin;
    }

    ImpulseAsset impulse(SourceSpan span) {
        const auto name = expect(Kind::word, "expected impulse name");
        if (name.text == "out" || name.text == "param" || name.text == "impulse") {
            throw Diagnostic(name.span, "reserved impulse name");
        }
        expect(Kind::assign, "expected '='");
        const auto kind = expect(Kind::word, "expected pcm(rate, samples...)");
        if (kind.text != "pcm") throw Diagnostic(kind.span, "expected pcm(rate, samples...)");
        const auto values = arguments(max_impulse_samples + 1);
        if (values.size() < 2) throw Diagnostic(span, "expected sample rate and at least one impulse tap");
        const auto& rate = values.front();
        if (!rate.reference.empty() || rate.value < 8000 || rate.value > 192000 || std::floor(rate.value) != rate.value) {
            throw Diagnostic(rate.span, "impulse sample rate must be an integer from 8000 to 192000");
        }
        ImpulseAsset asset{complete(span), std::string(name.text), static_cast<std::uint32_t>(rate.value), {}};
        asset.samples.reserve(values.size() - 1);
        for (std::size_t i = 1; i < values.size(); ++i) {
            const auto& tap = values[i];
            if (!tap.reference.empty()) throw Diagnostic(tap.span, "impulse taps must be numeric literals");
            asset.samples.push_back(tap.value);
        }
        if (!valid_impulse(asset.samples, asset.rate)) throw Diagnostic(span, "impulse exceeds its extent or gain bound");
        return asset;
    }

    std::vector<Argument> arguments(std::size_t limit) {
        expect(Kind::open, "expected '('");
        std::vector<Argument> result;
        if (token_.kind != Kind::close) {
            for (;;) {
                if (result.size() == limit) throw Diagnostic(token_.span, "too many arguments");
                if (token_.kind == Kind::number) {
                    const auto value = expect(Kind::number, "expected number");
                    result.push_back({value.span, {}, number(value)});
                } else {
                    const auto ref = expect(Kind::word, "expected a signal name or numeric literal");
                    result.push_back({ref.span, std::string(ref.text), 0});
                }
                if (token_.kind != Kind::comma) break;
                expect(Kind::comma, "expected ','");
            }
        }
        expect(Kind::close, "expected ')'");
        return result;
    }

    Lexer lexer_;
    Token token_;
};
}

Graph parse_graph(std::string_view source) {
    return Parser(source).run();
}
}
