#import "views.hpp"

#include "resonant/spectral.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
NSColor* ink() { return [NSColor colorWithSRGBRed:.83 green:.89 blue:.92 alpha:1]; }
NSColor* muted() { return [NSColor colorWithSRGBRed:.53 green:.65 blue:.71 alpha:1]; }
NSColor* mint() { return [NSColor colorWithSRGBRed:.31 green:.91 blue:.79 alpha:1]; }
NSColor* amber() { return [NSColor colorWithSRGBRed:1 green:.77 blue:.44 alpha:1]; }
NSColor* panel() { return [NSColor colorWithSRGBRed:.07 green:.10 blue:.13 alpha:1]; }
NSColor* tile() { return [NSColor colorWithSRGBRed:.12 green:.17 blue:.21 alpha:1]; }

NSString* ns(std::string_view text) {
    auto* result = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    return result != nil ? result : @"";
}

void label(NSString* text, NSPoint point, CGFloat size, NSColor* color, bool strong = false) {
    [text drawAtPoint:point withAttributes:@{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:size weight:strong ? NSFontWeightSemibold : NSFontWeightRegular],
        NSForegroundColorAttributeName: color
    }];
}

void dot(NSPoint point, NSColor* color, CGFloat size = 7) {
    [color setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(point.x-size/2, point.y-size/2, size, size)] fill];
}

struct Node {
    std::string name;
    std::string detail;
    std::vector<std::string> from;
    bool audio;
    bool active;
};

constexpr CGFloat width = 167;
constexpr CGFloat height = 52;
constexpr CGFloat row_height = 76;

NSRect node_rect(std::size_t index) {
    const CGFloat lane = static_cast<CGFloat>(index % 2);
    const CGFloat row = static_cast<CGFloat>(index / 2);
    return NSMakeRect(28 + lane * 202, 37 + row * row_height, width, height);
}

NSPoint input_point(NSRect rect, std::size_t index) {
    return NSMakePoint(NSMinX(rect), NSMinY(rect) + 13 + 13 * static_cast<CGFloat>(index));
}

NSPoint output_point(NSRect rect) { return NSMakePoint(NSMaxX(rect), NSMidY(rect)); }

constexpr std::array<unsigned short, 13> keys{0,13,1,14,2,3,17,5,16,4,32,38,40};
constexpr std::array<const char*, 13> letters{"A","W","S","E","D","F","T","G","Y","H","U","J","K"};
}

@implementation RGraphView {
    std::vector<Node> _nodes;
    std::string _wire;
}

- (BOOL)isFlipped { return YES; }

- (void)updateGraph:(const resonant::Graph&)graph compilation:(const resonant::Compilation&)compiled {
    _wire.clear();
    _nodes.clear();
    for (const auto& expr : graph.expressions) {
        Node node{expr.name, expr.operation, {}, false, false};
        if (expr.type == resonant::Declaration::parameter) node.detail = "parameter";
        if (expr.type == resonant::Declaration::literal) node.detail = "constant";
        for (const auto& arg : expr.arguments) node.from.push_back(arg.reference);
        for (std::size_t pc = 0; pc < compiled.schedule.size(); ++pc) {
            if (compiled.schedule[pc].name == expr.name) {
                const auto& ins = compiled.program.program().instructions[pc];
                node.audio = ins.rate == resonant::Rate::audio;
                node.active = true;
                node.detail += " · r" + std::to_string(ins.dst);
                break;
            }
        }
        _nodes.push_back(std::move(node));
    }
    Node terminal{"out", "stereo output", {}, true, true};
    for (const auto& arg : graph.outputs) terminal.from.push_back(arg.reference);
    _nodes.push_back(std::move(terminal));
    const CGFloat content = 45 + static_cast<CGFloat>((_nodes.size()+1)/2) * row_height;
    [self setFrameSize:NSMakeSize(430, std::max<CGFloat>(content, NSHeight(self.superview.bounds)))];
    [self setAccessibilityLabel:@"Typed signal graph. Use connection controls for keyboard access."];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)rect {
    [panel() setFill];
    (void)rect;
    NSRectFill(self.bounds);
    label(@"SIGNAL FLOW", NSMakePoint(27, 10), 11, mint(), true);
    label(_wire.empty() ? @"Click an output then an input" : [@"Route " stringByAppendingString:ns(_wire)],
          NSMakePoint(176, 10), 10, muted());
    for (std::size_t index = 0; index < _nodes.size(); ++index) {
        const auto& node = _nodes[index];
        for (std::size_t port = 0; port < node.from.size(); ++port) {
            const auto it = std::ranges::find(_nodes, node.from[port], &Node::name);
            if (it == _nodes.end()) continue;
            const auto origin = static_cast<std::size_t>(it - _nodes.begin());
            const auto a = output_point(node_rect(origin));
            const auto b = input_point(node_rect(index), port);
            const auto bend = std::max<CGFloat>(27, std::abs(b.x-a.x)*.22);
            NSBezierPath* curve = [NSBezierPath bezierPath];
            [curve moveToPoint:a];
            [curve curveToPoint:b controlPoint1:NSMakePoint(a.x+bend, a.y) controlPoint2:NSMakePoint(b.x-bend, b.y)];
            NSColor* color = it->audio ? mint() : amber();
            [[color colorWithAlphaComponent:_wire == it->name ? .9 : .39] setStroke];
            curve.lineWidth = _wire == it->name ? 2.4 : 1.25;
            [curve stroke];
        }
    }
    for (std::size_t index = 0; index < _nodes.size(); ++index) {
        const auto& node = _nodes[index];
        const auto bounds = node_rect(index);
        NSBezierPath* box = [NSBezierPath bezierPathWithRoundedRect:bounds xRadius:6 yRadius:6];
        [tile() setFill];
        [box fill];
        [(_wire == node.name ? mint() : [muted() colorWithAlphaComponent:.32]) setStroke];
        [box stroke];
        const auto title = node.name.substr(0, 18);
        label(ns(title), NSMakePoint(NSMinX(bounds)+13, NSMinY(bounds)+7), 12, node.active ? ink() : muted(), true);
        label(ns(node.detail.substr(0, 21)), NSMakePoint(NSMinX(bounds)+13, NSMinY(bounds)+29), 10, muted());
        if (node.name != "out") dot(output_point(bounds), node.audio ? mint() : amber(), 8);
        for (std::size_t port = 0; port < node.from.size(); ++port) {
            dot(input_point(bounds, port), node.from[port].empty() ? muted() : ink(), 6);
        }
    }
}

- (void)mouseDown:(NSEvent*)event {
    const auto point = [self convertPoint:event.locationInWindow fromView:nil];
    for (std::size_t index = 0; index < _nodes.size(); ++index) {
        const auto bounds = node_rect(index);
        const auto& node = _nodes[index];
        const auto end = output_point(bounds);
        if (node.name != "out" && std::hypot(point.x-end.x, point.y-end.y) < 12) {
            _wire = node.name;
            [self setNeedsDisplay:YES];
            return;
        }
        for (std::size_t port = 0; !_wire.empty() && port < node.from.size(); ++port) {
            const auto entry = input_point(bounds, port);
            if (std::hypot(point.x-entry.x, point.y-entry.y) < 11 && self.connection) {
                const auto source = std::exchange(_wire, {});
                self.connection(ns(source), ns(node.name), port);
                [self setNeedsDisplay:YES];
                return;
            }
        }
        if (NSPointInRect(point, bounds) && self.selection) {
            self.selection(ns(node.name));
            return;
        }
    }
    _wire.clear();
    [self setNeedsDisplay:YES];
}
@end

@implementation RScopeView {
    std::array<float, 64> _wave;
    std::array<float, 256> _history;
    std::array<float, 128> _spectrum;
    std::unique_ptr<resonant::Fft> _fft;
    std::size_t _cursor;
    std::uint32_t _rate;
}

- (BOOL)isFlipped { return YES; }

- (void)pushTrace:(const resonant::Trace&)trace rate:(std::uint32_t)rate {
    if (!_fft) _fft = std::make_unique<resonant::Fft>(_history.size());
    if (trace.scope_size == 0) return;
    _rate = rate * trace.scope_size / trace.frames;
    _wave = trace.scope;
    for (std::size_t i = 0; i < trace.scope_size; ++i) {
        _history[_cursor] = trace.scope[i];
        _cursor = (_cursor + 1) % _history.size();
    }
    std::array<std::complex<float>, 256> bins{};
    for (std::size_t i = 0; i < bins.size(); ++i) {
        const auto window = 0.5F - 0.5F * std::cos(2.0F * std::numbers::pi_v<float> *
                                               static_cast<float>(i) / static_cast<float>(bins.size() - 1));
        bins[i] = _history[(_cursor + i) % _history.size()] * window;
    }
    (void)_fft->transform(bins);
    for (std::size_t i = 0; i < _spectrum.size(); ++i) {
        const auto db = 20.0F * std::log10(std::max(std::abs(bins[i]) / 64.0F, 1e-5F));
        _spectrum[i] = std::clamp((db + 84.0F) / 84.0F, 0.0F, 1.0F);
    }
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)rect {
    [panel() setFill];
    (void)rect;
    NSRectFill(self.bounds);
    const auto full = NSWidth(self.bounds);
    const auto h = NSHeight(self.bounds);
    const auto half = full / 2;
    label(@"OSCILLOSCOPE  /  LEFT", NSMakePoint(15, 7), 11, mint(), true);
    label([NSString stringWithFormat:@"SPECTRUM  /  0–%.1f kHz", _rate / 2000.0], NSMakePoint(half+13, 7), 11, mint(), true);
    [[muted() colorWithAlphaComponent:.23] setStroke];
    [NSBezierPath strokeLineFromPoint:NSMakePoint(half, 6) toPoint:NSMakePoint(half, h-6)];
    [NSBezierPath strokeLineFromPoint:NSMakePoint(15, (h+22)/2) toPoint:NSMakePoint(half-15, (h+22)/2)];
    [mint() setStroke];
    NSBezierPath* wave = [NSBezierPath bezierPath];
    for (std::size_t i = 0; i < _wave.size(); ++i) {
        const auto x = 15 + (half-30) * static_cast<CGFloat>(i) / static_cast<CGFloat>(_wave.size()-1);
        const auto y = (h+22)/2 - (h-29)/2 * std::clamp(_wave[i]*4, -1.0F, 1.0F);
        if (i == 0) [wave moveToPoint:NSMakePoint(x,y)];
        else [wave lineToPoint:NSMakePoint(x,y)];
    }
    [wave stroke];
    [[mint() colorWithAlphaComponent:.74] setFill];
    const auto bar = (half-29) / static_cast<CGFloat>(_spectrum.size());
    for (std::size_t i = 0; i < _spectrum.size(); ++i) {
        const auto value = _spectrum[i] * (h-29);
        NSRectFill(NSMakeRect(half+14 + bar*static_cast<CGFloat>(i), h-7-value, std::max<CGFloat>(1,bar*.76), value));
    }
}
@end

@implementation RKeysView {
    std::array<bool, 13> _pressed;
    NSInteger _mouse;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) _mouse = -1;
    return self;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }
- (void)change:(std::size_t)index on:(bool)on {
    if (index >= keys.size() || _pressed[index] == on) return;
    _pressed[index] = on;
    if (self.noteEvent) self.noteEvent(static_cast<std::uint8_t>(60 + index), on);
    [self setNeedsDisplay:YES];
}
- (void)releaseNotes {
    for (std::size_t i = 0; i < keys.size(); ++i) [self change:i on:false];
    _mouse = -1;
}
- (BOOL)resignFirstResponder {
    [self releaseNotes];
    return [super resignFirstResponder];
}
- (void)keyDown:(NSEvent*)event {
    if ((event.modifierFlags & NSEventModifierFlagCommand) != 0) { [super keyDown:event]; return; }
    const auto it = std::ranges::find(keys, event.keyCode);
    if (it == keys.end()) { [super keyDown:event]; return; }
    [self change:static_cast<std::size_t>(it-keys.begin()) on:true];
}
- (void)keyUp:(NSEvent*)event {
    const auto it = std::ranges::find(keys, event.keyCode);
    if (it != keys.end()) [self change:static_cast<std::size_t>(it-keys.begin()) on:false];
}
- (void)mouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    const auto p = [self convertPoint:event.locationInWindow fromView:nil];
    const auto key = static_cast<NSInteger>(std::floor(p.x * keys.size() / NSWidth(self.bounds)));
    if (key >= 0 && key < static_cast<NSInteger>(keys.size())) {
        _mouse = key;
        [self change:static_cast<std::size_t>(key) on:true];
    }
}
- (void)mouseUp:(NSEvent*)event {
    (void)event;
    if (_mouse >= 0) [self change:static_cast<std::size_t>(_mouse) on:false];
    _mouse = -1;
}
- (void)drawRect:(NSRect)rect {
    [panel() setFill];
    (void)rect;
    NSRectFill(self.bounds);
    const auto w = NSWidth(self.bounds) / keys.size();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto x = static_cast<CGFloat>(i)*w;
        [(_pressed[i] ? mint() : tile()) setFill];
        NSRectFill(NSMakeRect(x+1, 2, w-2, NSHeight(self.bounds)-4));
        label(ns(letters[i]), NSMakePoint(x+w/2-3, 6), 11, _pressed[i] ? panel() : ink(), true);
    }
}
@end
