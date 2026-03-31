#include "resonant/graph.hpp"
#include "resonant/midi.hpp"
#include "resonant/session.hpp"
#include "test.hpp"

using namespace resonant;

int main() {
    const auto session = parse_session(
        "resonant-session 1\nlength 5760\ntempo 0 500000\ntempo 2880 625000\n"
        "note 960 960 0 69 0.5\ncontrol 2880 gain 0.1 64\n"
        "source\nparam gain=0.5; p=poly_sine(.005,.12); o=mul(p,gain); out(o);\n");
    const auto current = encode_session(session);
    CHECK(current.starts_with("resonant-session 2\n"));
    CHECK(encode_session(parse_session(current)) == current);
    const TempoMap tempo(session.tempo);
    CHECK(tempo.frame(2880, 48000) == 72000);
    CHECK(tempo.frame(3840, 48000) == 102000);
    CHECK(tempo.tick(102000, 48000) == 3840);
    CHECK(tempo.frame(2880, 44100) == 66150);

    auto loop = session;
    loop.loop = {960, 2880, 3};
    const auto timeline = schedule(loop, 48000);
    CHECK(timeline.frames == 258000);
    CHECK(timeline.events[0].type == EventType::panic && timeline.events[0].frame == 24000);
    CHECK(timeline.events[1].type == EventType::note_on && timeline.events[1].frame == 24000);
    const auto program = compile(loop.source);
    const auto first = render_offline(program.program, timeline, 48000, 32, Execution::scalar);
    CHECK(first.peak > 0.02F && first.samples.size() == 2 * timeline.frames);
    CHECK(first.samples[47999] == 0 && first.samples[48000] == 0 && first.samples[48002] != 0);
    for (const auto block : {64U, 128U, 256U, 512U}) {
        CHECK(render_offline(program.program, timeline, 48000, block).samples == first.samples);
    }

    auto broken = current;
    broken.replace(17, 1, "8");
    rejects([&] { (void)parse_session(broken); });
    rejects([&] { (void)parse_session("resonant-session 2\nlength 10\ntempo 1 500000\nsource\na"); });
    rejects([&] { (void)parse_session("resonant-session 2\nlength 10\ntempo 0 500000\nloop 3 11 2\nsource\na"); });
    auto excess = timeline;
    excess.events.assign(events_per_callback + 1, {0, EventType::panic});
    rejects([&] { (void)render_offline(program.program, excess, 48000, 128); });

    Midi1 midi;
    CHECK(!midi.push(0x91, 10));
    CHECK(!midi.push(69, 11));
    CHECK(!midi.push(0xf8, 12));
    const auto on = midi.push(127, 13);
    CHECK(on && on->frame == 11 && on->channel == 1 && on->type == EventType::note_on && on->value == 1);
    CHECK(!midi.push(69, 55));
    const auto off = midi.push(0, 56);
    CHECK(off && off->frame == 55 && off->type == EventType::note_off);
    CHECK(!midi.push(0xf0, 70));
    CHECK(!midi.push(0x7f, 71));
    CHECK(!midi.push(0xf7, 72));
    CHECK(!midi.push(0x30, 73));
    CHECK(midi.malformed() == 1 && midi.ignored() == 3);
}
