#include "resonant/session.hpp"

#include "resonant/graph.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace resonant {
namespace {
constexpr std::uint64_t time_units_per_second = 1000000ULL * ticks_per_quarter;
constexpr std::size_t max_arrangement_events = 16384;
constexpr std::size_t max_scheduled_events = 131072;

void check(bool valid, const char* reason) {
    if (!valid) throw std::runtime_error(reason);
}

void check_rate(std::uint32_t rate) {
    check(rate >= 8000 && rate <= 192000, "unsupported sample rate");
}

bool identifier(std::string_view name) {
    const auto initial = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    return !name.empty() && name.size() <= 63 && initial(name.front()) &&
           std::ranges::all_of(name, [&](char c) { return initial(c) || (c >= '0' && c <= '9'); });
}

void validate(const Session& session) {
    check(session.length > 0 && session.length <= max_tick, "session length out of bounds");
    check(!session.source.empty() && session.source.size() <= max_source_bytes, "invalid session source extent");
    check(session.notes.size() <= max_arrangement_events && session.automation.size() <= max_arrangement_events &&
          session.pedal.size() <= max_arrangement_events, "arrangement exceeds event limit");
    for (const auto& n : session.notes) {
        check(n.tick < session.length && n.duration > 0 && n.duration <= session.length - n.tick &&
              n.channel < 16 && n.pitch < 128 && std::isfinite(n.velocity) && n.velocity > 0 && n.velocity <= 1,
              "invalid note");
    }
    for (const auto& a : session.automation) {
        check(a.tick < session.length && identifier(a.parameter) &&
              valid_event({0, EventType::parameter, 0, 0, name_key(a.parameter), a.value, a.ramp_frames}),
              "invalid automation");
    }
    for (const auto& p : session.pedal) check(p.tick < session.length && p.channel < 16, "invalid pedal event");
    for (const auto& t : session.tempo) check(t.tick < session.length, "tempo occurs outside session");
    const auto& loop = session.loop;
    check(loop.passes >= 1 && loop.passes <= 64, "invalid loop pass count");
    if (loop.passes > 1 || loop.begin != 0 || loop.end != 0) {
        check(loop.begin < loop.end && loop.end <= session.length, "invalid loop extent");
    }
}

std::string_view line(std::string_view& rest) {
    const auto end = rest.find('\n');
    auto current = rest.substr(0, end);
    rest = end == rest.npos ? std::string_view{} : rest.substr(end + 1);
    if (current.ends_with('\r')) current.remove_suffix(1);
    return current;
}

struct Fields {
    std::array<std::string_view, 6> values{};
    std::size_t size{};
};

Fields split(std::string_view text) {
    Fields result;
    const auto comment = text.find('#');
    if (comment != text.npos) text = text.substr(0, comment);
    while (!text.empty()) {
        const auto start = text.find_first_not_of(" \t");
        if (start == text.npos) break;
        text.remove_prefix(start);
        const auto end = text.find_first_of(" \t");
        check(result.size < result.values.size(), "too many session fields");
        result.values[result.size++] = text.substr(0, end);
        if (end == text.npos) break;
        text.remove_prefix(end);
    }
    return result;
}

template<class T>
T number(std::string_view field) {
    T value{};
    const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
    check(error == std::errc{} && end == field.data() + field.size(), "invalid numeric session field");
    return value;
}

int priority(EventType type) {
    switch (type) {
    case EventType::panic: return 0;
    case EventType::all_off:
    case EventType::note_off: return 1;
    case EventType::sustain: return 2;
    case EventType::parameter: return 3;
    case EventType::note_on: return 4;
    }
    return 5;
}
}

TempoMap::TempoMap(std::span<const Tempo> changes) {
    check(!changes.empty() && changes.size() <= 1024 && changes.front().tick == 0, "tempo map must start at tick zero");
    std::uint64_t time = 0;
    for (const auto tempo : changes) {
        check(tempo.tick <= max_tick && tempo.microseconds_per_quarter >= 10000 &&
              tempo.microseconds_per_quarter <= 60000000, "invalid tempo change");
        if (!segments_.empty()) {
            const auto previous = segments_.back().change;
            check(tempo.tick > previous.tick, "tempo ticks must strictly increase");
            time += (tempo.tick - previous.tick) * previous.microseconds_per_quarter;
        }
        segments_.push_back({tempo, time});
    }
}

std::uint64_t TempoMap::frame(std::uint64_t tick_value, std::uint32_t rate) const {
    check_rate(rate);
    check(tick_value <= max_tick, "tick exceeds supported range");
    const auto found = std::ranges::upper_bound(segments_, tick_value, {}, [](const auto& segment) { return segment.change.tick; });
    const auto& entry = *(found - 1);
    const auto elapsed = entry.elapsed + (tick_value - entry.change.tick) * entry.change.microseconds_per_quarter;
    return elapsed / time_units_per_second * rate +
           (elapsed % time_units_per_second * rate + time_units_per_second / 2) / time_units_per_second;
}

std::uint64_t TempoMap::tick(std::uint64_t frame_value, std::uint32_t rate) const {
    check_rate(rate);
    std::uint64_t low = 0;
    std::uint64_t high = max_tick;
    while (low < high) {
        const auto mid = low + (high - low + 1) / 2;
        if (frame(mid, rate) <= frame_value) low = mid;
        else high = mid - 1;
    }
    return low;
}

Session parse_session(std::string_view contents) {
    check(contents.size() <= max_session_bytes, "session exceeds byte limit");
    const auto header = line(contents);
    const bool legacy = header == "resonant-session 1";
    check(legacy || header == "resonant-session 2", "unsupported session version");
    Session session;
    session.tempo.clear();
    bool length = false;
    bool loop = false;
    std::uint32_t line_number = 1;
    while (!contents.empty()) {
        ++line_number;
        const auto current = line(contents);
        if (current == "source") {
            session.source = contents;
            break;
        }
        try {
            const auto fields = split(current);
            const auto& value = fields.values;
            if (fields.size == 0) continue;
            if (value[0] == "length" && fields.size == 2 && !length) {
                session.length = number<std::uint64_t>(value[1]);
                length = true;
            } else if (value[0] == "tempo" && fields.size == 3 && session.tempo.size() < 1024) {
                session.tempo.push_back({number<std::uint64_t>(value[1]), number<std::uint32_t>(value[2])});
            } else if (value[0] == "loop" && fields.size == 4 && !legacy && !loop) {
                session.loop = {number<std::uint64_t>(value[1]), number<std::uint64_t>(value[2]),
                                number<std::uint32_t>(value[3])};
                loop = true;
            } else if (value[0] == "note" && fields.size == 6 && session.notes.size() < max_arrangement_events) {
                session.notes.push_back({number<std::uint64_t>(value[1]), number<std::uint64_t>(value[2]),
                                         number<std::uint8_t>(value[3]), number<std::uint8_t>(value[4]),
                                         number<float>(value[5])});
            } else if (value[0] == "control" && fields.size == 5 && session.automation.size() < max_arrangement_events) {
                session.automation.push_back({number<std::uint64_t>(value[1]), std::string(value[2]),
                                               number<float>(value[3]), number<std::uint32_t>(value[4])});
            } else if (value[0] == "pedal" && fields.size == 4 && session.pedal.size() < max_arrangement_events) {
                check(value[3] == "0" || value[3] == "1", "invalid pedal state");
                session.pedal.push_back({number<std::uint64_t>(value[1]), number<std::uint8_t>(value[2]), value[3] == "1"});
            } else {
                throw std::runtime_error("unknown, duplicate or excess session directive");
            }
        } catch (const std::runtime_error& error) {
            throw std::runtime_error("session line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    check(length, "missing session length");
    (void)TempoMap(session.tempo);
    validate(session);
    return session;
}

std::string encode_session(const Session& session) {
    (void)TempoMap(session.tempo);
    validate(session);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "resonant-session 2\nlength " << session.length << '\n';
    for (const auto& t : session.tempo) out << "tempo " << t.tick << ' ' << t.microseconds_per_quarter << '\n';
    if (session.loop.end > 0) out << "loop " << session.loop.begin << ' ' << session.loop.end << ' ' << session.loop.passes << '\n';
    for (const auto& n : session.notes) {
        out << "note " << n.tick << ' ' << n.duration << ' ' << static_cast<unsigned>(n.channel) << ' '
            << static_cast<unsigned>(n.pitch) << ' ' << n.velocity << '\n';
    }
    for (const auto& a : session.automation) out << "control " << a.tick << ' ' << a.parameter << ' ' << a.value << ' ' << a.ramp_frames << '\n';
    for (const auto& p : session.pedal) out << "pedal " << p.tick << ' ' << static_cast<unsigned>(p.channel) << ' ' << p.down << '\n';
    out << "source\n" << session.source;
    auto encoded = out.str();
    check(encoded.size() <= max_session_bytes, "encoded session exceeds byte limit");
    return encoded;
}

Timeline schedule(const Session& session, std::uint32_t rate) {
    validate(session);
    const TempoMap tempo(session.tempo);
    Timeline result{};
    const auto append = [&](Event event) {
        check(result.events.size() < max_scheduled_events, "expanded event budget exceeded");
        result.events.push_back(event);
    };
    const auto slice = [&](std::uint64_t begin, std::uint64_t end) {
        const auto start = tempo.frame(begin, rate);
        const auto length = tempo.frame(end, rate) - start;
        const auto at = [&](std::uint64_t tick) { return result.frames + tempo.frame(tick, rate) - start; };
        for (const auto& n : session.notes) {
            if (n.tick >= begin && n.tick < end) {
                const auto off = std::max(at(n.tick) + 1, at(std::min(end, n.tick + n.duration)));
                if (at(n.tick) < result.frames + length) {
                    append({at(n.tick), EventType::note_on, n.channel, n.pitch, 0, n.velocity});
                    append({off, EventType::note_off, n.channel, n.pitch});
                }
            }
        }
        for (const auto& p : session.pedal) {
            if (p.tick >= begin && p.tick < end && at(p.tick) < result.frames + length) {
                append({at(p.tick), EventType::sustain, p.channel, 0, 0, p.down ? 1.0F : 0.0F});
            }
        }
        for (const auto& a : session.automation) {
            if (a.tick >= begin && a.tick < end && at(a.tick) < result.frames + length) {
                append({at(a.tick), EventType::parameter, 0, 0, name_key(a.parameter), a.value, a.ramp_frames});
            }
        }
        result.frames += length;
    };
    if (session.loop.passes == 1) {
        slice(0, session.length);
    } else {
        slice(0, session.loop.begin);
        for (std::uint32_t pass = 0; pass < session.loop.passes; ++pass) {
            if (result.frames != 0) append({result.frames, EventType::panic});
            slice(session.loop.begin, session.loop.end);
        }
        if (session.loop.end != session.length) {
            append({result.frames, EventType::panic});
            slice(session.loop.end, session.length);
        }
    }
    std::erase_if(result.events, [&](const auto& e) { return e.frame >= result.frames; });
    std::ranges::stable_sort(result.events, [](const auto& a, const auto& b) {
        return a.frame == b.frame ? priority(a.type) < priority(b.type) : a.frame < b.frame;
    });
    return result;
}

OfflineRender render_offline(const VerifiedProgram& program, const Timeline& timeline, std::uint32_t rate,
                             std::size_t block_size, Execution execution) {
    check(timeline.frames > 0 && timeline.frames <= max_render_frames, "render frame budget exceeded");
    check(block_size >= 1 && block_size <= max_block, "invalid offline block size");
    check(timeline.events.size() <= max_scheduled_events, "offline event budget exceeded");
    for (std::size_t i = 0; i < timeline.events.size(); ++i) {
        const auto& event = timeline.events[i];
        check(event.frame < timeline.frames && (i == 0 || event.frame >= timeline.events[i-1].frame),
              "events must be monotonic and inside the render");
        if (event.type == EventType::parameter) {
            const auto& params = program.program().parameters;
            check(std::ranges::any_of(params, [&](const auto& p) { return p.key == event.key; }),
                  "automation refers to a parameter absent from the verified patch");
        }
    }
    Engine engine(program, rate, execution);
    OfflineRender result{};
    result.samples.resize(static_cast<std::size_t>(2 * timeline.frames));
    std::size_t next = 0;
    for (std::uint64_t frame = 0; frame < timeline.frames;) {
        auto end = std::min(frame + block_size, timeline.frames);
        const auto remaining = timeline.events.size() - next;
        if (remaining > events_per_callback && timeline.events[next + events_per_callback].frame < end) {
            end = timeline.events[next + events_per_callback].frame;
            check(end > frame, "too many events at one sample");
        }
        while (next < timeline.events.size() && timeline.events[next].frame < end) {
            check(engine.enqueue(timeline.events[next++]) == Enqueue::accepted, "offline event rejected");
        }
        auto output = std::span(result.samples).subspan(static_cast<std::size_t>(frame * 2),
                                                        static_cast<std::size_t>((end - frame) * 2));
        engine.render(output);
        Trace trace{};
        check(engine.poll(trace) && trace.first_frame == frame && !trace.event_budget_exhausted &&
              trace.late_events == 0 && trace.absent_parameters == 0, "offline clock or queue failure");
        result.instructions += trace.render.instructions;
        result.clipped += trace.render.clipped;
        result.peak = std::max(result.peak, trace.render.peak);
        frame = end;
    }
    return result;
}
}
