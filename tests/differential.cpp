#include "resonant/graph.hpp"
#include "resonant/vm.hpp"
#include "test.hpp"

#include <algorithm>
#include <random>

using namespace resonant;

namespace {
struct Node {
    int operation;
    std::size_t x;
    std::size_t y;
    bool audio;
};

float reference(int op, float x, float y) {
    if (op == 0) return x + y;
    if (op == 1) return x - y;
    if (op == 2) return x * y;
    if (op == 3) return std::min(std::max(x, -std::abs(y)), std::abs(y));
    return x;
}
}

int main() {
    std::mt19937 rng(0x7342);
    const std::array operations{"add", "sub", "mul", "clip", "audio"};
    std::uint64_t folded = 0;
    std::uint64_t discarded = 0;
    for (int graph = 0; graph < 10000; ++graph) {
        const auto controls = 6 + rng() % 17;
        std::vector<Node> nodes{{-1, 0, 0, false}, {-1, 0, 0, false}, {-1, 0, 0, true}};
        for (std::size_t i = nodes.size(); i < controls; ++i) {
            const auto op = static_cast<int>(rng() % operations.size());
            const auto x = rng() % i;
            const auto y = rng() % i;
            nodes.push_back({op, x, y, op == 4 || nodes[x].audio || nodes[y].audio});
        }
        const auto label = [](std::size_t n) { return "v" + std::to_string(n); };
        const auto a = rng() % nodes.size();
        const auto b = rng() % nodes.size();
        std::string source = "out(left,right); left=audio(" + label(a) + "); right=audio(" + label(b) + ");";
        for (std::size_t i = nodes.size(); i-- > 3;) {
            const auto& n = nodes[i];
            source += label(i) + '=' + operations[static_cast<std::size_t>(n.operation)] + '(' + label(n.x);
            if (n.operation != 4) source += ',' + label(n.y);
            source += ");";
        }
        source += "param v0=0.0625; v1=-0.25; v2=audio(0.125);";
        const auto compiled = compile(source);
        folded += compiled.folded;
        discarded += compiled.eliminated;
        const auto& p = compiled.program.program();
        Vm machine(compiled.program, 48000, graph % 2 == 0 ? Execution::scalar : Execution::block);
        constexpr std::array control_values{0.0625F, -0.375F};
        for (const float control : control_values) {
            const auto param = std::ranges::find(p.parameters, name_key("v0"), &Parameter::key);
            if (param != p.parameters.end()) {
                CHECK(machine.event({0, EventType::parameter, 0, 0, name_key("v0"), control}));
            }
            std::vector<float> values{control, -0.25F, 0.125F};
            for (std::size_t i = values.size(); i < nodes.size(); ++i) {
                const auto& n = nodes[i];
                values.push_back(reference(n.operation, values[n.x], values[n.y]));
            }
            std::array<float, 16> output{};
            CHECK(machine.render(output).instructions == 8 * p.instructions.size());
            for (std::size_t frame = 0; frame < output.size() / 2; ++frame) {
                CHECK(output[frame*2] == std::clamp(values[a], -1.0F, 1.0F));
                CHECK(output[frame*2+1] == std::clamp(values[b], -1.0F, 1.0F));
            }
        }
        for (std::size_t i = 0; i < compiled.schedule.size(); ++i) {
            const auto& name = compiled.schedule[i].name;
            if (name.starts_with("v")) {
                const auto index = std::stoul(name.substr(1));
                CHECK((p.instructions[i].rate == Rate::audio) == nodes[index].audio);
            }
        }
    }
    CHECK(folded > 100 && discarded > 10000);
    std::cout << "10000 independent graph comparisons, " << folded << " folds and " << discarded << " eliminated values\n";
}
