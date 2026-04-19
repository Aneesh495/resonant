#include "resonant/graph.hpp"

#include "numeric.hpp"

#include <algorithm>
#include <bit>
#include <functional>
#include <map>
#include <queue>
#include <utility>

namespace resonant {
namespace {
using NodeId = std::size_t;

struct Vertex {
    SourceSpan span;
    std::string name;
    Op op = Op::literal;
    Rate rate = Rate::control;
    std::array<NodeId, 3> input{};
    std::uint8_t inputs = 0;
    std::uint32_t words = 0;
    float value = 0;
};

class Compiler {
public:
    explicit Compiler(const Graph& graph) : graph_(graph) {}

    Compilation run() {
        declare();
        for (NodeId i = 0; i < graph_.expressions.size(); ++i) lower(i);
        outputs_[0] = resolve(graph_.outputs[0]);
        outputs_[1] = graph_.outputs.size() == 2 ? resolve(graph_.outputs[1]) : outputs_[0];
        const auto order = topological_order();
        infer(order);
        fold(order);
        return emit(order);
    }

private:
    void declare() {
        for (const auto& expression : graph_.expressions) {
            if (!names_.emplace(expression.name, vertices_.size()).second) {
                throw Diagnostic(expression.span, "duplicate name '" + expression.name + "'");
            }
            vertices_.push_back({expression.span, expression.name});
        }
    }

    NodeId resolve(const Argument& arg) {
        if (!arg.reference.empty()) {
            const auto found = names_.find(arg.reference);
            if (found == names_.end()) throw Diagnostic(arg.span, "unknown signal '" + arg.reference + "'");
            return found->second;
        }
        const auto [where, inserted] = literals_.emplace(std::bit_cast<std::uint32_t>(arg.value), vertices_.size());
        if (inserted) {
            Vertex v{arg.span, "$" + std::to_string(where->second)};
            v.value = numeric::bounded(arg.value);
            vertices_.push_back(std::move(v));
        }
        return where->second;
    }

    void lower(NodeId id) {
        const auto& expression = graph_.expressions[id];
        auto vertex = vertices_[id];
        if (expression.type != Declaration::node) {
            vertex.op = expression.type == Declaration::parameter ? Op::parameter : Op::literal;
            vertex.value = numeric::bounded(expression.arguments[0].value);
        } else if (expression.operation == "delay" || expression.operation == "align") {
            if (expression.arguments.size() != 2) throw Diagnostic(vertex.span, expression.operation + " requires signal and frame count");
            const auto& extent = expression.arguments[1];
            if (!extent.reference.empty() || extent.value < 1 || extent.value > static_cast<float>(max_delay) ||
                std::floor(extent.value) != extent.value) {
                throw Diagnostic(extent.span, "frame count must be a bounded positive integer literal");
            }
            vertex.op = expression.operation == "delay" ? Op::delay_read : Op::align;
            vertex.words = static_cast<std::uint32_t>(extent.value);
            vertex.input[0] = resolve(expression.arguments[0]);
            vertex.inputs = 1;
        } else {
            vertex.op = source_op(expression);
            const auto& contract = opcode(vertex.op);
            const auto count = expression.arguments.size();
            const bool default_phase = vertex.op == Op::sine && count == 1;
            if (!default_phase && count != contract.inputs) {
                throw Diagnostic(vertex.span, expression.operation + " requires " + std::to_string(contract.inputs) + " inputs");
            }
            vertex.inputs = contract.inputs;
            vertex.words = contract.state_words;
            for (std::size_t i = 0; i < count; ++i) vertex.input[i] = resolve(expression.arguments[i]);
            if (default_phase) vertex.input[1] = resolve({vertex.span, {}, 0});
        }
        vertices_[id] = std::move(vertex);
    }

    static Op source_op(const Expression& expression) {
        for (auto i = static_cast<unsigned>(Op::add); i < static_cast<unsigned>(Op::count); ++i) {
            const auto op = static_cast<Op>(i);
            if (op != Op::output && opcode(op).name == expression.operation) return op;
        }
        throw Diagnostic(expression.span, "unknown operation '" + expression.operation + "'");
    }

    std::vector<NodeId> topological_order() const {
        std::vector<std::size_t> incoming(vertices_.size());
        std::vector<std::vector<NodeId>> successors(vertices_.size());
        std::priority_queue<NodeId, std::vector<NodeId>, std::greater<>> ready;
        for (NodeId i = 0; i < vertices_.size(); ++i) {
            const auto& vertex = vertices_[i];
            if (vertex.op != Op::delay_read) {
                incoming[i] = vertex.inputs;
                for (std::size_t j = 0; j < vertex.inputs; ++j) successors[vertex.input[j]].push_back(i);
            }
            if (incoming[i] == 0) ready.push(i);
        }
        std::vector<NodeId> result;
        while (!ready.empty()) {
            const auto id = ready.top();
            ready.pop();
            result.push_back(id);
            for (const auto dependent : successors[id]) {
                if (--incoming[dependent] == 0) ready.push(dependent);
            }
        }
        if (result.size() != vertices_.size()) {
            const auto remaining = std::ranges::find_if(incoming, [](std::size_t n) { return n != 0; });
            const auto id = static_cast<NodeId>(remaining - incoming.begin());
            throw Diagnostic(vertices_[id].span, "algebraic cycle or dependency on one; use an explicit delay");
        }
        return result;
    }

    void infer(const std::vector<NodeId>& order) {
        for (const auto id : order) {
            auto& vertex = vertices_[id];
            const auto rule = opcode(vertex.op).rule;
            if (rule == Rule::first_audio && vertices_[vertex.input[0]].rate != Rate::audio) {
                throw Diagnostic(vertex.span, "first input requires a mono audio signal");
            }
            if (rule == Rule::first_audio || rule == Rule::audio) vertex.rate = Rate::audio;
            if (rule == Rule::join) {
                for (std::size_t i = 0; i < vertex.inputs; ++i) {
                    if (vertices_[vertex.input[i]].rate == Rate::audio) vertex.rate = Rate::audio;
                }
            }
        }
        for (const auto& vertex : vertices_) {
            if (vertex.op == Op::delay_read && vertices_[vertex.input[0]].rate != Rate::audio) {
                throw Diagnostic(vertex.span, "delay feedback requires a mono audio signal");
            }
        }
        for (std::size_t i = 0; i < graph_.outputs.size(); ++i) {
            if (vertices_[outputs_[i]].rate != Rate::audio) {
                throw Diagnostic(graph_.outputs[i].span, "output requires audio rate; use audio(value) for an explicit lift");
            }
        }
    }

    void fold(const std::vector<NodeId>& order) {
        for (const auto id : order) {
            auto& v = vertices_[id];
            if (opcode(v.op).rule != Rule::join || vertices_[v.input[0]].op != Op::literal ||
                vertices_[v.input[1]].op != Op::literal) continue;
            v.value = numeric::arithmetic(v.op, vertices_[v.input[0]].value, vertices_[v.input[1]].value);
            v.op = Op::literal;
            v.inputs = 0;
            ++folded_;
        }
    }

    std::vector<bool> reachable() const {
        std::vector<bool> seen(vertices_.size());
        std::vector<NodeId> pending(outputs_.begin(), outputs_.end());
        while (!pending.empty()) {
            const auto id = pending.back();
            pending.pop_back();
            if (seen[id]) continue;
            seen[id] = true;
            const auto& v = vertices_[id];
            for (std::size_t i = 0; i < v.inputs; ++i) pending.push_back(v.input[i]);
        }
        return seen;
    }

    Compilation emit(const std::vector<NodeId>& order) const {
        const auto live = reachable();
        std::vector<std::size_t> uses(vertices_.size());
        for (NodeId id = 0; id < vertices_.size(); ++id) {
            if (!live[id]) continue;
            const auto& v = vertices_[id];
            for (std::size_t i = 0; i < v.inputs; ++i) ++uses[v.input[i]];
        }
        for (const auto id : outputs_) ++uses[id];

        Program program{};
        std::vector<ScheduleEntry> schedule;
        std::vector<std::size_t> produced_at(vertices_.size());
        std::vector<std::uint16_t> registers(vertices_.size());
        std::array<bool, max_registers> allocated{};
        std::vector<NodeId> delays;
        for (const auto id : order) {
            if (!live[id]) continue;
            const auto& v = vertices_[id];
            const auto free = std::ranges::find(allocated, false);
            if (free == allocated.end()) throw Diagnostic(v.span, "live register budget exceeded");
            const auto dst = static_cast<std::uint16_t>(free - allocated.begin());
            allocated[dst] = true;
            program.registers = std::max(program.registers, static_cast<std::uint16_t>(dst + 1));
            registers[id] = dst;
            Instruction ins{v.op, v.rate, dst};
            if (v.words != 0) {
                if (program.states.size() == max_states) throw Diagnostic(v.span, "state slot budget exceeded");
                ins.slot = static_cast<std::uint16_t>(program.states.size());
                program.states.push_back({name_key(v.name), v.op, v.words});
            }
            if (v.op == Op::parameter) {
                if (program.parameters.size() == max_parameters) throw Diagnostic(v.span, "parameter budget exceeded");
                ins.slot = static_cast<std::uint16_t>(program.parameters.size());
                program.parameters.push_back({name_key(v.name), v.value});
            }
            if (v.op == Op::literal) ins.immediate = v.value;
            ScheduleEntry row{v.span, v.name, {}, program.instructions.size()};
            if (v.op == Op::delay_read) {
                delays.push_back(id);
            } else {
                for (std::size_t i = 0; i < v.inputs; ++i) {
                    const auto src = v.input[i];
                    ins.in[i] = registers[src];
                    row.dependencies.push_back(vertices_[src].name);
                    schedule[produced_at[src]].last_use = program.instructions.size();
                    if (--uses[src] == 0) allocated[registers[src]] = false;
                }
            }
            produced_at[id] = program.instructions.size();
            program.instructions.push_back(ins);
            schedule.push_back(std::move(row));
        }
        for (const auto id : delays) {
            const auto& v = vertices_[id];
            const auto source = v.input[0];
            const auto slot = program.instructions[produced_at[id]].slot;
            schedule[produced_at[source]].last_use = program.instructions.size();
            schedule[produced_at[id]].dependencies.push_back(vertices_[source].name);
            program.instructions.push_back({Op::delay_write, Rate::audio, 0, {registers[source]}, slot});
            schedule.push_back({v.span, v.name + ".commit", {vertices_[source].name}, program.instructions.size() - 1});
        }
        for (const auto id : outputs_) schedule[produced_at[id]].last_use = program.instructions.size();
        program.instructions.push_back({Op::output, Rate::audio, 0, {registers[outputs_[0]], registers[outputs_[1]]}});
        schedule.push_back({graph_.output_span, "out", {vertices_[outputs_[0]].name, vertices_[outputs_[1]].name},
                            program.instructions.size() - 1});
        const auto eliminated = static_cast<std::size_t>(std::ranges::count(live, false));
        try {
            return {verify(std::move(program)), std::move(schedule), eliminated, folded_};
        } catch (const std::runtime_error& error) {
            throw Diagnostic(graph_.output_span, error.what());
        }
    }

    const Graph& graph_;
    std::vector<Vertex> vertices_;
    std::map<std::string, NodeId, std::less<>> names_;
    std::map<std::uint32_t, NodeId> literals_;
    std::array<NodeId, 2> outputs_{};
    std::size_t folded_ = 0;
};
}

Compilation compile(std::string_view source) {
    const auto graph = parse_graph(source);
    return Compiler(graph).run();
}
}
