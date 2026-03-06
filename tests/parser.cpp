#include "resonant/graph.hpp"
#include "test.hpp"

using namespace resonant;

int main() {
    const auto graph = parse_graph("# test\r\nparam f = +4.4e2;\nx = sine(f,-.125); out(x);");
    CHECK(graph.expressions.size() == 2 && graph.outputs.size() == 1);
    const auto& parameter = graph.expressions.front();
    CHECK(parameter.arguments[0].value == 440 && parameter.span.line == 2);
    CHECK(parameter.span.offset == 8 && parameter.span.length == 17);
    const auto& sine = graph.expressions.back();
    CHECK(sine.operation == "sine" && sine.arguments[0].reference == "f");
    CHECK(sine.arguments[1].value == -0.125F);
    CHECK(graph.output_span.column == 20);
    for (const std::string_view source : {"", "x = 1;", "out();", "out(1,2,3);", "x = 1e99; out(x);",
                                         "x = 1.; out(x); out(x);", "param out = 1; out(out);",
                                         "out(10", "out(1,);", "x=foo(1,2,3,4); out(x);"}) {
        rejects([&] { (void)parse_graph(source); });
    }
    rejects([] { (void)parse_graph(std::string(max_source_bytes + 1, '#')); });
    try {
        (void)parse_graph("# line one\n  out(x!);");
        CHECK(false);
    } catch (const Diagnostic& error) {
        CHECK(error.span().line == 2 && error.span().column == 8);
    }
}
