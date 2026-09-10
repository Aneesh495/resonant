#import "views.hpp"

#include "midi_input.hpp"

#include "resonant/device.hpp"
#include "resonant/session.hpp"
#include "resonant/wave.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <mach/mach_time.h>

namespace {
void control_surface(NSControl* control, NSString* title, bool primary, bool popup) {
    const auto bounds = NSInsetRect(control.bounds, 1, 2);
    const auto selected = primary || control.cell.highlighted;
    auto* fill = selected ? [NSColor colorWithSRGBRed:.11 green:.41 blue:.44 alpha:1] :
                           [NSColor colorWithSRGBRed:.15 green:.20 blue:.25 alpha:1];
    auto* stroke = [NSColor colorWithSRGBRed:.27 green:.38 blue:.43 alpha:1];
    if (control.window.firstResponder == control) stroke = NSColor.systemTealColor;
    NSBezierPath* box = [NSBezierPath bezierPathWithRoundedRect:bounds xRadius:5 yRadius:5];
    [fill setFill];
    [box fill];
    [stroke setStroke];
    [box stroke];
    NSDictionary* style = @{
        NSFontAttributeName: [NSFont systemFontOfSize:12 weight:selected ? NSFontWeightSemibold : NSFontWeightMedium],
        NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:.88 green:.92 blue:.95 alpha:1]
    };
    const auto extent = [title sizeWithAttributes:style];
    [title drawAtPoint:NSMakePoint(popup ? 10 : (NSWidth(bounds)-extent.width)/2+1,
                                   (NSHeight(control.bounds)-extent.height)/2) withAttributes:style];
    if (popup) [@"⌄" drawAtPoint:NSMakePoint(NSWidth(bounds)-16, (NSHeight(control.bounds)-extent.height)/2)
                   withAttributes:style];
}
}

@interface RButton : NSButton
@end
@implementation RButton
- (void)drawRect:(NSRect)dirty { (void)dirty; control_surface(self, self.title, [self.title isEqualToString:@"Apply patch"], false); }
@end

@interface RPopup : NSPopUpButton
@end
@implementation RPopup
- (void)drawRect:(NSRect)dirty { (void)dirty; control_surface(self, self.titleOfSelectedItem, false, true); }
@end

namespace {
using namespace resonant;
using Path = std::filesystem::path;

NSString* ns(std::string_view value) {
    auto* text = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
    return text != nil ? text : @"";
}
std::string str(NSString* value) {
    const auto* bytes = value.UTF8String;
    return bytes != nullptr ? bytes : "";
}

NSTextField* text(NSString* string, CGFloat size = 12, bool accent = false) {
    auto* label = [NSTextField labelWithString:string];
    label.font = [NSFont monospacedSystemFontOfSize:size weight:accent ? NSFontWeightSemibold : NSFontWeightRegular];
    label.textColor = accent ? [NSColor colorWithSRGBRed:.32 green:.91 blue:.79 alpha:1] : NSColor.secondaryLabelColor;
    return label;
}

NSButton* button(NSString* title, id target, SEL action, NSString* key = @"") {
    auto* item = [[RButton alloc] initWithFrame:NSZeroRect];
    item.title = title;
    item.target = target;
    item.action = action;
    item.keyEquivalent = key;
    return item;
}

NSStackView* row(NSArray<NSView*>* controls) {
    auto* stack = [NSStackView stackViewWithViews:controls];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.spacing = 6;
    stack.alignment = NSLayoutAttributeCenterY;
    return stack;
}

void minimum(NSView* view, CGFloat width) { [view.widthAnchor constraintGreaterThanOrEqualToConstant:width].active = YES; }

NSScrollView* scroll(NSView* view) {
    auto* scroller = [[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,440,320)];
    scroller.hasVerticalScroller = YES;
    scroller.hasHorizontalScroller = YES;
    scroller.autohidesScrollers = YES;
    scroller.borderType = NSNoBorder;
    scroller.documentView = view;
    return scroller;
}

NSTextView* editor(bool editable) {
    auto* view = [[NSTextView alloc] initWithFrame:NSMakeRect(0,0,440,320)];
    view.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    view.backgroundColor = [NSColor colorWithSRGBRed:.09 green:.12 blue:.15 alpha:1];
    view.textColor = [NSColor colorWithSRGBRed:.87 green:.91 blue:.94 alpha:1];
    view.insertionPointColor = NSColor.systemTealColor;
    view.textContainerInset = NSMakeSize(10, 9);
    view.verticallyResizable = YES;
    view.horizontallyResizable = NO;
    view.textContainer.widthTracksTextView = YES;
    view.autoresizingMask = NSViewWidthSizable;
    view.richText = NO;
    view.allowsUndo = editable;
    view.automaticQuoteSubstitutionEnabled = NO;
    view.automaticDashSubstitutionEnabled = NO;
    view.automaticSpellingCorrectionEnabled = NO;
    view.editable = editable;
    return view;
}

std::string listing(const Compilation& compiled) {
    std::ostringstream out;
    out << "PC  LINE   NODE                 RATE    REGISTER / LAST USE\n";
    for (std::size_t pc = 0; pc < compiled.schedule.size(); ++pc) {
        const auto& row = compiled.schedule[pc];
        const auto& ins = compiled.program.program().instructions[pc];
        out.width(2);
        out << pc << "  ";
        out.width(4);
        out << row.span.line << "   ";
        out.width(20);
        out << std::left << row.name << std::right << ' ';
        out << (ins.rate == Rate::audio ? "audio   " : "control ");
        if (opcode(ins.op).writes_register) out << 'r' << ins.dst << " → " << row.last_use;
        out << "  " << opcode(ins.op).name << '\n';
    }
    out << '\n' << compiled.program.work_per_frame() << " work units per frame, " << compiled.eliminated
        << " eliminated values, " << compiled.folded << " folds\n";
    return out.str();
}

struct CaptureLineage {
    std::uint64_t first_frame{};
    std::uint64_t first_program{};
    std::uint64_t last_program{};
    std::uint64_t frames{};
    std::uint64_t digest{14695981039346656037ULL};

    void append(const CaptureBlock& block, std::size_t count) {
        const auto fade = std::min<std::uint64_t>(count, block.fade_frames);
        if (frames == 0) {
            first_frame = block.first_frame;
            first_program = fade != 0 ? block.fading_program : block.program;
        }
        const std::array fields{block.first_frame, block.generation, block.program,
                                block.fading_program, fade, static_cast<std::uint64_t>(count)};
        for (auto value : fields) {
            for (unsigned i = 0; i < 8; ++i) {
                digest = (digest ^ (value & 255U)) * 1099511628211ULL;
                value >>= 8U;
            }
        }
        last_program = block.program;
        frames += count;
    }

    [[nodiscard]] std::string comment() const {
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << "Resonant capture frames=" << frames << " first_frame=" << first_frame
             << std::hex << " first_program_id=" << first_program << " last_program_id=" << last_program
             << " lineage_id=" << digest;
        return text.str();
    }
};

void wave_output(const Path& path, const std::vector<float>& samples, std::uint32_t rate,
                 std::string_view identity) {
    const std::vector<double> pcm(samples.begin(), samples.end());
    atomic_write(path, encode_wave(pcm, {Encoding::pcm24, rate, 2, 0, identity}));
}

std::uint64_t export_session(const Path& path, const Session& session, std::uint32_t rate,
                             const std::atomic<bool>* cancel = nullptr) {
    const auto c = compile(session.source);
    const auto timeline = schedule(session, rate);
    const auto result = render_offline(c.program, timeline, rate, 128, Execution::block, cancel);
    if (cancel && cancel->load()) throw std::runtime_error("export cancelled");
    const auto identity = render_comment(timeline.frames, name_key(encode_session(session)), program_key(c.program));
    wave_output(path, result.samples, rate, identity);
    return timeline.frames;
}
}

@interface REditorPanel : NSView
- (instancetype)initWithSource:(NSView*)source listing:(NSView*)listing;
@end

@implementation REditorPanel {
    NSView* _source;
    NSView* _listing;
    NSButton* _patch;
    NSButton* _code;
}
- (instancetype)initWithSource:(NSView*)source listing:(NSView*)listing {
    if ((self = [super initWithFrame:NSMakeRect(0,0,480,340)])) {
        _source = source;
        _listing = listing;
        _patch = button(@"Patch source", self, @selector(showSource:));
        _code = button(@"Compiled schedule", self, @selector(showCode:));
        for (NSView* view in @[_source,_listing,_patch,_code]) [self addSubview:view];
        [self showSource:nil];
        [self setFrameSize:self.frame.size];
    }
    return self;
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    _source.frame = NSMakeRect(1, 0, size.width-1, std::max<CGFloat>(0, size.height-30));
    _listing.frame = _source.frame;
    _patch.frame = NSMakeRect(9, size.height-28, 117, 25);
    _code.frame = NSMakeRect(133, size.height-28, 150, 25);
}
- (void)showSource:(id)sender {
    (void)sender;
    _listing.hidden = YES;
    _source.hidden = NO;
}
- (void)showCode:(id)sender {
    (void)sender;
    _source.hidden = YES;
    _listing.hidden = NO;
}
@end

@interface RWorkbenchLayout : NSView
@property(nonatomic, copy) NSArray<NSView*>* items;
- (void)placeSubviews;
@end

@implementation RWorkbenchLayout
- (void)drawRect:(NSRect)rect {
    [[NSColor colorWithSRGBRed:.075 green:.09 blue:.12 alpha:1] setFill];
    NSRectFill(NSIntersectionRect(self.bounds, rect));
}
- (void)setItems:(NSArray<NSView*>*)items {
    _items = [items copy];
    for (NSView* view in _items) [self addSubview:view];
    [self placeSubviews];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self placeSubviews];
}

- (void)placeSubviews {
    if (_items.count != 8) return;
    const auto width = NSWidth(self.bounds) - 26;
    auto top = NSHeight(self.bounds) - 10;
    for (NSUInteger i = 0; i < 3; ++i) {
        top -= 29;
        _items[i].frame = NSMakeRect(13, top, width, 29);
        top -= 4;
    }
    NSSplitView* split = (NSSplitView*)_items[3];
    split.frame = NSMakeRect(13, 188, width, std::max<CGFloat>(200, top - 188));
    _items[4].frame = NSMakeRect(13, 81, width, 100);
    _items[5].frame = NSMakeRect(13, 45, width, 29);
    _items[6].frame = NSMakeRect(13, 25, width, 15);
    _items[7].frame = NSMakeRect(13, 8, width, 15);
}
@end

@interface RWorkbench : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTextViewDelegate, NSMenuDelegate>
- (instancetype)initWithInput:(NSString*)input smoke:(NSString*)smoke;
@end

@implementation RWorkbench {
    NSWindow* _window;
    NSTextView* _source;
    NSTextView* _listing;
    NSTextField* _message;
    NSTextField* _metrics;
    NSTextField* _tempo;
    NSButton* _audio;
    NSButton* _record;
    NSButton* _export;
    NSSlider* _level;
    NSSlider* _parameterValue;
    NSPopUpButton* _from;
    NSPopUpButton* _to;
    NSPopUpButton* _port;
    NSPopUpButton* _parameter;
    NSPopUpButton* _rate;
    NSPopUpButton* _output;
    NSPopUpButton* _midiMenu;
    RGraphView* _graph;
    RScopeView* _scope;
    RKeysView* _keys;
    NSString* _input;
    NSString* _smoke;
    NSTimer* _timer;
    NSTimer* _eventTimer;
    Session _session;
    std::unique_ptr<Compilation> _compiled;
    std::unique_ptr<Engine> _engine;
    std::unique_ptr<Device> _device;
    std::vector<int> _devices;
    std::unique_ptr<apple::MidiInput> _midi;
    std::uint32_t _midiEndpoint;
    std::uint64_t _midiStarted;
    std::uint64_t _midiLost;
    std::uint64_t _midiAdjusted;
    std::uint64_t _midiInboxLost;
    std::vector<Event> _midiPending;
    double _midiSecondsPerTick;
    Timeline _timeline;
    std::size_t _scoreIndex;
    std::uint64_t _scoreBase;
    std::uint64_t _lastEvent;
    CaptureLineage _captureLineage;
    std::uint32_t _captureRate;
    std::uint64_t _captureOverflow;
    std::uint64_t _generation;
    std::uint64_t _underruns;
    std::uint64_t _pollCount;
    double _latency;
    Trace _lastTrace;
    std::vector<float> _recorded;
    std::shared_ptr<std::atomic<bool>> _cancelExport;
    bool _scorePlaying;
    bool _recording;
    bool _captureFailed;
}

- (instancetype)initWithInput:(NSString*)input smoke:(NSString*)smoke {
    if ((self = [super init])) { _input = input; _smoke = smoke; }
    return self;
}

- (void)message:(std::string_view)message error:(bool)error {
    _message.stringValue = ns(message);
    _message.textColor = error ? NSColor.systemOrangeColor : NSColor.secondaryLabelColor;
}

- (void)buildWindow {
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1120,730)
         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
         backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"Resonant · Signal Workbench";
    _window.contentMinSize = NSMakeSize(890, 570);
    _window.delegate = self;
    _window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    [_window center];

    _source = editor(true);
    _source.delegate = self;
    [_source setAccessibilityLabel:@"Patch source"];
    _listing = editor(false);
    [_listing setAccessibilityLabel:@"Compiled instructions and register lifetimes"];
    _graph = [[RGraphView alloc] initWithFrame:NSMakeRect(0,0,430,330)];
    _scope = [[RScopeView alloc] initWithFrame:NSZeroRect];
    [_scope setAccessibilityLabel:@"Oscilloscope and spectrum of the VM output"];
    _keys = [[RKeysView alloc] initWithFrame:NSZeroRect];
    [_keys setAccessibilityLabel:@"Play notes with A W S E D F T G Y H U J K"];

    _audio = button(@"Start muted", self, @selector(toggleAudio:));
    _record = button(@"Record", self, @selector(toggleRecord:));
    _export = button(@"Export WAV", self, @selector(exportWave:));
    _rate = [[RPopup alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_rate addItemsWithTitles:@[@"44100", @"48000", @"96000"]];
    [_rate selectItemWithTitle:@"48000"];
    _rate.target = self;
    _rate.action = @selector(reconfigure:);
    [_rate setAccessibilityLabel:@"Sample rate"];
    _output = [[RPopup alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_output addItemWithTitle:@"Default output"];
    _devices.push_back(-1);
    _output.target = self;
    _output.action = @selector(reconfigure:);
    [_output setAccessibilityLabel:@"Audio output device"];
    _midiMenu = [[RPopup alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_midiMenu addItemWithTitle:@"MIDI Off"];
    _midiMenu.menu.delegate = self;
    _midiMenu.target = self;
    _midiMenu.action = @selector(selectMidi:);
    [_midiMenu setAccessibilityLabel:@"MIDI input source"];
    _level = [NSSlider sliderWithValue:0 minValue:0 maxValue:.2 target:self action:@selector(monitor:)];
    [_level setAccessibilityLabel:@"Monitor gain"];
    minimum(_level, 64);
    _tempo = [NSTextField textFieldWithString:@"120"];
    _tempo.target = self;
    _tempo.action = @selector(tempo:);
    [_tempo setAccessibilityLabel:@"Initial tempo in beats per minute"];
    minimum(_tempo, 57);
    _message = text(@"Ready");
    _message.lineBreakMode = NSLineBreakByTruncatingTail;
    _metrics = text(@"AUDIO OFF", 11, true);
    _metrics.lineBreakMode = NSLineBreakByTruncatingTail;

    _from = [[RPopup alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _to = [[RPopup alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _port = [[RPopup alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _parameter = [[RPopup alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _parameter.target = self;
    _parameter.action = @selector(selectParameter:);
    [_from setAccessibilityLabel:@"Source node"];
    [_to setAccessibilityLabel:@"Target node"];
    [_port setAccessibilityLabel:@"Target input"];
    [_parameter setAccessibilityLabel:@"Parameter"];
    [_port addItemsWithTitles:@[@"Input 1", @"Input 2", @"Input 3"]];
    for (NSView* view in @[_from,_to,_parameter]) minimum(view, 82);
    _parameterValue = [NSSlider sliderWithValue:0 minValue:0 maxValue:1 target:self action:@selector(changeParameter:)];
    [_parameterValue setAccessibilityLabel:@"Parameter value"];
    minimum(_parameterValue, 90);
    auto* brand = text(@"RESONANT", 17, true);
    minimum(brand, 99);
    auto* upper = row(@[brand, button(@"Open…", self, @selector(openSession:), @"o"),
        button(@"Save…", self, @selector(saveSession:), @"s"),
        button(@"Apply patch", self, @selector(applyPatch:), @"\r"), _audio, _rate, _output, text(@"Monitor"), _level]);
    auto* transport = row(@[button(@"Play score", self, @selector(playScore:)),
        button(@"Stop", self, @selector(stopScore:)), text(@"BPM"), _tempo,
        _record, _export, button(@"Focus keys", self, @selector(focusKeys:)), _midiMenu,
        button(@"Import Asset…", self, @selector(importAsset:))]);
    auto* route = row(@[text(@"ROUTE", 10, true), _from, text(@"→"), _to, _port,
        button(@"Connect", self, @selector(connect:)), text(@"CONTROL", 10, true), _parameter, _parameterValue]);

    auto* pane = [[REditorPanel alloc] initWithSource:scroll(_source) listing:scroll(_listing)];

    auto* split = [[NSSplitView alloc] initWithFrame:NSMakeRect(0,0,1000,360)];
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    [split addArrangedSubview:scroll(_graph)];
    [split addArrangedSubview:pane];

    auto* layout = [[RWorkbenchLayout alloc] initWithFrame:_window.contentView.bounds];
    layout.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _window.contentView = layout;
    layout.items = @[upper, transport, route, split, _scope, _keys, _metrics, _message];
    [split setHoldingPriority:NSLayoutPriorityDefaultLow+10 forSubviewAtIndex:0];
    [split setHoldingPriority:NSLayoutPriorityDefaultLow forSubviewAtIndex:1];

    __weak RWorkbench* weak = self;
    _graph.connection = ^(NSString* from, NSString* target, NSUInteger port) {
        [weak routeFrom:from to:target port:port];
    };
    _graph.selection = ^(NSString* name) {
        [weak selectSource:name];
    };
    _keys.noteEvent = ^(std::uint8_t note, bool on) {
        [weak playNote:note on:on];
    };
    [_window makeKeyAndOrderFront:nil];
    [_window layoutIfNeeded];
    [split setPosition:435 ofDividerAtIndex:0];
    [_window makeFirstResponder:_source];
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [self buildWindow];
    try {
        if (Device::built()) {
            _device = std::make_unique<Device>();
            for (const auto& entry : _device->devices()) {
                [_output addItemWithTitle:ns(entry.name)];
                _devices.push_back(entry.index);
            }
        }
        NSString* path = _input != nil ? _input : [[NSBundle mainBundle] pathForResource:@"ember-motif" ofType:@"rsn"];
        if (!path) throw std::runtime_error("starter session missing");
        [self loadPath:Path(str(path))];
    } catch (const std::exception& error) { [self message:error.what() error:true]; }
    _timer = [NSTimer timerWithTimeInterval:1.0/30.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    [NSApp activateIgnoringOtherApps:YES];
    if (_smoke) [self performSelector:@selector(startSmoke) withObject:nil afterDelay:.25];
}

- (void)loadPath:(const Path&)path {
    const auto contents = read_text(path, max_session_bytes);
    Session next = path.extension() == ".rpatch" ? Session{} : parse_session(contents);
    if (path.extension() == ".rpatch") next.source = contents;
    auto compiled = compile(next.source);
    [self stopScore:nil];
    _session = std::move(next);
    _source.string = ns(_session.source);
    _tempo.doubleValue = 60000000.0 / _session.tempo.front().microseconds_per_quarter;
    [self accept:std::move(compiled)];
    [self message:"Session loaded" error:false];
}

- (void)accept:(Compilation)compilation {
    const auto graph = parse_graph(_session.source);
    _compiled = std::make_unique<Compilation>(std::move(compilation));
    if (_engine) _generation = _engine->publish(_compiled->program);
    [_graph updateGraph:graph compilation:*_compiled];
    _listing.string = ns(listing(*_compiled));
    [_from removeAllItems];
    [_to removeAllItems];
    [_parameter removeAllItems];
    for (const auto& expr : graph.expressions) {
        auto* name = ns(expr.name);
        [_from addItemWithTitle:name];
        if (expr.type == Declaration::node) [_to addItemWithTitle:name];
        if (expr.type == Declaration::parameter && std::ranges::any_of(_compiled->program.program().parameters,
            [&](const auto& p) { return p.key == name_key(expr.name); })) [_parameter addItemWithTitle:name];
    }
    for (const auto& asset : graph.assets) [_from addItemWithTitle:ns(asset.name)];
    [_to addItemWithTitle:@"out"];
    [self selectParameter:nil];
    [self updateMetrics];
}

- (bool)applySource:(std::string)source {
    try {
        auto compiled = compile(source);
        _session.source = std::move(source);
        [self accept:std::move(compiled)];
        [self message:_engine ? "Verified patch queued at the next available block" : "Patch compiled and verified" error:false];
        return true;
    } catch (const std::exception& error) {
        [self message:error.what() error:true];
        return false;
    }
}

- (void)textDidChange:(NSNotification*)notification {
    (void)notification;
    [self message:"Source edited. Apply to compile and publish." error:false];
}

- (void)applyPatch:(id)sender {
    (void)sender;
    (void)[self applySource:str(_source.string)];
}

- (bool)routeFrom:(NSString*)from to:(NSString*)target port:(NSUInteger)port {
    try {
        if (!from.length || !target.length) throw std::runtime_error("select a source and a target");
        auto source = str(_source.string);
        const auto graph = parse_graph(source);
        const auto found = std::ranges::find(graph.expressions, str(target), &Expression::name);
        const auto* args = [target isEqualToString:@"out"] ? &graph.outputs :
                           found != graph.expressions.end() ? &found->arguments : nullptr;
        if (args == nullptr || port >= args->size()) throw std::runtime_error("target has no such input port");
        const auto span = (*args)[port].span;
        source.replace(span.offset, span.length, str(from));
        if (![self applySource:source]) return false;
        _source.string = ns(source);
        return true;
    } catch (const std::exception& error) {
        [self message:error.what() error:true];
        return false;
    }
}

- (void)connect:(id)sender {
    (void)sender;
    (void)[self routeFrom:_from.titleOfSelectedItem to:_to.titleOfSelectedItem port:static_cast<NSUInteger>(_port.indexOfSelectedItem)];
}

- (void)selectSource:(NSString*)name {
    try {
        const auto source = str(_source.string);
        const auto graph = parse_graph(source);
        const auto found = std::ranges::find(graph.expressions, str(name), &Expression::name);
        const auto asset = std::ranges::find(graph.assets, str(name), &SignalAsset::name);
        const auto span = [name isEqualToString:@"out"] ? graph.output_span :
                          found != graph.expressions.end() ? found->span :
                          asset != graph.assets.end() ? asset->span : SourceSpan{};
        const auto prefix = ns(std::string_view(source).substr(0, span.offset)).length;
        const auto length = ns(std::string_view(source).substr(span.offset, span.length)).length;
        [_source setSelectedRange:NSMakeRange(prefix,length)];
        [_source scrollRangeToVisible:NSMakeRange(prefix,length)];
    } catch (const std::exception& error) { [self message:error.what() error:true]; }
}

- (void)selectParameter:(id)sender {
    (void)sender;
    if (!_compiled) return;
    const auto key = name_key(str(_parameter.titleOfSelectedItem));
    const auto& params = _compiled->program.program().parameters;
    const auto found = std::ranges::find(params, key, &Parameter::key);
    _parameterValue.enabled = found != params.end();
    if (found != params.end()) {
        const auto range = std::max(1.0, 2.0 * std::abs(found->initial));
        _parameterValue.minValue = found->initial >= 0 ? 0 : -range;
        _parameterValue.maxValue = range;
        _parameterValue.doubleValue = found->initial;
    }
}

- (std::uint32_t)selectedRate { return static_cast<std::uint32_t>(_rate.titleOfSelectedItem.intValue); }

- (void)startAudio {
    if (!_compiled || !_device) throw std::runtime_error("verified patch and PortAudio are required");
    auto next = std::make_unique<Engine>(_compiled->program, [self selectedRate]);
    const auto index = static_cast<std::size_t>(_output.indexOfSelectedItem);
    _device->open(*next, {_devices.at(index), 128, true});
    _midiStarted = mach_absolute_time();
    _engine = std::move(next);
    _midiPending.reserve(512);
    if (_midi) {
        Event discarded;
        for (unsigned i = 0; i < 512 && _midi->poll(discarded); ++i) {}
        _midiInboxLost = _midi->dropped();
    }
    _eventTimer = [NSTimer timerWithTimeInterval:.004 target:self selector:@selector(pumpEvents:)
                                        userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:_eventTimer forMode:NSRunLoopCommonModes];
    _lastEvent = 0;
    _generation = 1;
    _underruns = 0;
    _latency = _device->status().output_latency;
    _level.doubleValue = 0;
    _audio.title = @"Stop audio";
    [self message:"Audio running muted. Raise Monitor to listen." error:false];
}

- (void)stopAudio {
    if (_recording) {
        _captureFailed = true;
        _recording = false;
        _record.title = @"Record";
    }
    _scorePlaying = false;
    _midiPending.clear();
    [_eventTimer invalidate];
    _eventTimer = nil;
    [_keys releaseNotes];
    if (_device) _device->close();
    _engine.reset();
    _audio.title = @"Start muted";
    [self updateMetrics];
}

- (void)toggleAudio:(id)sender {
    (void)sender;
    try {
        if (_engine) [self stopAudio];
        else [self startAudio];
    } catch (const std::exception& error) { [self message:error.what() error:true]; }
}

- (void)reconfigure:(id)sender {
    (void)sender;
    if (_engine) {
        [self stopAudio];
        [self toggleAudio:nil];
    }
}

- (void)monitor:(id)sender {
    (void)sender;
    if (_engine) _device->gain(_level.floatValue);
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (menu != _midiMenu.menu) return;
    const auto sources = apple::MidiInput::sources();
    [_midiMenu removeAllItems];
    [_midiMenu addItemWithTitle:@"MIDI Off"];
    for (const auto& source : sources) {
        NSString* title = ns(source.name);
        if ([_midiMenu itemWithTitle:title] != nil) title = [title stringByAppendingFormat:@" (%u)", source.endpoint];
        [_midiMenu addItemWithTitle:title];
        [_midiMenu itemWithTitle:title].tag = source.endpoint;
    }
    [_midiMenu selectItemWithTag:_midiEndpoint];
    if (_midiEndpoint != 0 && _midiMenu.selectedItem.tag != _midiEndpoint) {
        [_midiMenu selectItemAtIndex:0];
        [self selectMidi:nil];
        [self message:"MIDI source disconnected" error:true];
    }
}

- (void)selectMidi:(id)sender {
    (void)sender;
    const auto selected = static_cast<std::uint32_t>(_midiMenu.selectedItem.tag);
    if (selected == _midiEndpoint) return;
    if (_midiEndpoint != 0) [self stopScore:nil];
    if (_midi) _midi->disconnect();
    _midiEndpoint = 0;
    try {
        if (selected == 0) {
            [self message:"MIDI input off" error:false];
            return;
        }
        if (!_midi) _midi = std::make_unique<apple::MidiInput>();
        _midiSecondsPerTick = apple::host_seconds_per_tick();
        if (_midiSecondsPerTick <= 0) throw std::runtime_error("host clock is unavailable");
        _midi->connect(selected);
        _midiEndpoint = selected;
        _midiInboxLost = 0;
        [self message:"MIDI 1.0 input connected" error:false];
    } catch (const std::exception& error) {
        [_midiMenu selectItemAtIndex:0];
        [self message:error.what() error:true];
    }
}

- (void)readMidiAt:(std::uint64_t)cursor {
    if (_midiEndpoint == 0) return;
    const auto lost = _midi->dropped();
    if (lost != _midiInboxLost) {
        _midiLost += lost - _midiInboxLost;
        _midiInboxLost = lost;
        Event discarded;
        for (unsigned i = 0; i < 512 && _midi->poll(discarded); ++i) {}
        [self stopScore:nil];
        [self message:"MIDI input overflow. Transport cleared to prevent stuck notes." error:true];
        return;
    }
    const auto anchor = _device->clock_anchor();
    if (!anchor) return;
    const auto rate = _engine->sample_rate();
    Event event;
    bool added = false;
    for (unsigned i = 0; i < 128 && _midi->poll(event); ++i) {
        if (event.frame < _midiStarted) continue;
        if (_midiPending.size() == 512) {
            ++_midiLost;
            [self stopScore:nil];
            [self message:"MIDI scheduling queue overflow. Transport cleared." error:true];
            return;
        }
        const auto frame = map_host_frame(*anchor, event.frame, _midiSecondsPerTick, rate,
                                          cursor + anchor->block, rate / 4, rate / 100);
        _midiAdjusted += frame.adjustment != ClockAdjustment::exact;
        event.frame = frame.frame;
        _midiPending.push_back(event);
        added = true;
    }
    if (added) std::stable_sort(_midiPending.begin(), _midiPending.end(),
                                [](const auto& a, const auto& b) { return a.frame < b.frame; });
}

- (void)pumpEvents:(NSTimer*)timer {
    (void)timer;
    if (!_engine || _engine->cancellation_pending()) return;
    const auto cursor = _engine->clock();
    [self readMidiAt:cursor];
    if (_engine->cancellation_pending()) return;
    const auto horizon = cursor + _engine->sample_rate() / (_midiEndpoint == 0 ? 20U : 100U);
    std::size_t consumed = 0;
    for (unsigned i = 0; i < 128; ++i) {
        const auto* midi = consumed < _midiPending.size() ? &_midiPending[consumed] : nullptr;
        const auto* score = _scorePlaying && _scoreIndex < _timeline.events.size() ? &_timeline.events[_scoreIndex] : nullptr;
        const auto score_frame = score ? _scoreBase + score->frame : UINT64_MAX;
        const bool use_midi = midi && midi->frame < score_frame;
        const auto frame = use_midi ? midi->frame : score_frame;
        if (frame >= horizon) break;
        auto event = use_midi ? *midi : *score;
        event.frame = std::max(frame, _lastEvent);
        if (use_midi && event.frame != frame) ++_midiAdjusted;
        if (_engine->enqueue(event) != Enqueue::accepted) {
            if (use_midi) ++_midiLost;
            [self stopScore:nil];
            [self message:"Event queue rejected input. Transport cleared." error:true];
            return;
        }
        _lastEvent = event.frame;
        if (use_midi) ++consumed;
        else ++_scoreIndex;
    }
    _midiPending.erase(_midiPending.begin(), _midiPending.begin() + static_cast<std::ptrdiff_t>(consumed));
    if (_scorePlaying && cursor >= _scoreBase + _timeline.frames) [self stopScore:nil];
}

- (bool)submit:(Event)event {
    if (!_engine) {
        [self message:"Start audio first" error:true];
        return false;
    }
    event.frame = std::max({_lastEvent, _engine->clock() + 128, event.frame});
    const auto status = _engine->enqueue(event);
    if (status != Enqueue::accepted) {
        [self message:status == Enqueue::cancellation_pending ? "Transport stop is pending" : "Event queue rejected input" error:true];
        return false;
    }
    _lastEvent = event.frame;
    return true;
}

- (void)playNote:(std::uint8_t)note on:(bool)on {
    (void)[self submit:{0, on ? EventType::note_on : EventType::note_off, 15, note, 0, on ? .7F : 0.0F}];
}

- (void)focusKeys:(id)sender { (void)sender; [_window makeFirstResponder:_keys]; }

- (void)changeParameter:(id)sender {
    (void)sender;
    (void)[self submit:{0, EventType::parameter, 0, 0, name_key(str(_parameter.titleOfSelectedItem)),
                       _parameterValue.floatValue, 256}];
}

- (void)setTempo {
    const auto bpm = _tempo.doubleValue;
    if (!std::isfinite(bpm) || bpm < 10 || bpm > 600) throw std::runtime_error("tempo must be 10 to 600 BPM");
    _session.tempo.front().microseconds_per_quarter = static_cast<std::uint32_t>(std::lround(60000000.0 / bpm));
}

- (void)tempo:(id)sender {
    (void)sender;
    try {
        [self setTempo];
        [self message:"Initial tempo saved in the arrangement" error:false];
    } catch (const std::exception& error) { [self message:error.what() error:true]; }
}

- (void)playScore:(id)sender {
    (void)sender;
    try {
        if (!_engine) [self startAudio];
        if (_scorePlaying) throw std::runtime_error("stop the current transport before restarting");
        [self setTempo];
        _timeline = schedule(_session, _engine->sample_rate());
        _scoreBase = std::max(_engine->clock()+1024, _lastEvent+1);
        _scoreIndex = 0;
        _scorePlaying = true;
        [self message:"Score scheduled against the audio sample clock" error:false];
    } catch (const std::exception& error) { [self message:error.what() error:true]; }
}

- (void)stopScore:(id)sender {
    (void)sender;
    _scorePlaying = false;
    _midiPending.clear();
    if (_engine) _engine->cancel_events();
    _lastEvent = 0;
}

- (void)startCapture {
    if (!_engine) throw std::runtime_error("start audio before recording");
    CaptureBlock block;
    while (_engine->poll_capture(block)) {}
    _recorded.clear();
    _captureLineage = {};
    _captureRate = _engine->sample_rate();
    _captureOverflow = _engine->capture_overflow();
    _captureFailed = false;
    _recording = true;
    _engine->capture(true);
    _record.title = @"Stop & save";
    [self message:"Recording the stereo VM mix" error:false];
}

- (void)drainCapture {
    CaptureBlock block;
    const auto capacity = static_cast<std::size_t>(_captureRate) * 60;
    while (_engine->poll_capture(block)) {
        const auto remaining = capacity - _captureLineage.frames;
        if (remaining == 0) continue;
        if (_captureLineage.frames != 0 &&
            _captureLineage.first_frame + _captureLineage.frames != block.first_frame) _captureFailed = true;
        const auto frames = std::min<std::uint64_t>(remaining, block.frames);
        const auto samples = static_cast<std::ptrdiff_t>(2 * frames);
        _recorded.insert(_recorded.end(), block.interleaved.begin(), block.interleaved.begin() + samples);
        _captureLineage.append(block, frames);
        if (frames == remaining) {
            _engine->capture(false);
            [self message:"Recording reached the 60-second buffer limit" error:false];
        }
    }
    _captureFailed |= _engine->capture_overflow() != _captureOverflow;
}

- (void)finishCapture {
    _engine->capture(false);
    [self drainCapture];
    _recording = false;
    _record.title = @"Record";
    if (_captureFailed || _recorded.empty()) throw std::runtime_error("recording is empty or contains a capture gap");
}

- (void)toggleRecord:(id)sender {
    (void)sender;
    try {
        if (!_recording) { [self startCapture]; return; }
        [self finishCapture];
        auto* panel = [NSSavePanel savePanel];
        panel.nameFieldStringValue = @"resonant-capture.wav";
        if ([panel runModal] != NSModalResponseOK) return;
        wave_output(Path(panel.URL.fileSystemRepresentation), _recorded, _captureRate, _captureLineage.comment());
        [self message:"Recorded WAV saved" error:false];
    } catch (const std::exception& error) { [self message:error.what() error:true]; }
}

- (void)openSession:(id)sender {
    (void)sender;
    auto* panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] != NSModalResponseOK) return;
    try { [self loadPath:Path(panel.URL.fileSystemRepresentation)]; }
    catch (const std::exception& error) { [self message:error.what() error:true]; }
}

- (void)importAsset:(id)sender {
    (void)sender;
    auto* panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories = NO;
    auto* kind = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 220, 30) pullsDown:NO];
    [kind addItemsWithTitles:@[@"Impulse response", @"Single-cycle wavetable"]];
    [kind setAccessibilityLabel:@"Asset type"];
    panel.accessoryView = kind;
    panel.accessoryViewDisclosed = YES;
    if ([panel runModal] != NSModalResponseOK) return;
    const bool table = kind.indexOfSelectedItem == 1;
    try {
        auto source = str(_source.string);
        const auto graph = parse_graph(source);
        const auto bytes = read_file(Path(panel.URL.fileSystemRepresentation), 2 * 1024 * 1024);
        const auto wave = parse_wave(bytes);
        for (std::size_t i = 1; i <= graph.expressions.size() + graph.assets.size() + 1; ++i) {
            const auto name = (table ? "table" : "ir") + std::to_string(i);
            if (std::ranges::find(graph.expressions, name, &Expression::name) != graph.expressions.end() ||
                std::ranges::find(graph.assets, name, &SignalAsset::name) != graph.assets.end()) continue;
            source += '\n' + (table ? wavetable_declaration(name, wave) : impulse_declaration(name, wave));
            if ([self applySource:source]) {
                _source.string = ns(source);
                const auto op = table ? "wavetable(hertz, phase, " : "convolve_ir(signal, mix, ";
                [self message:"Imported " + name + ". Use " + op + name + ")." error:false];
            }
            break;
        }
    } catch (const std::exception& error) { [self message:error.what() error:true]; }
}

- (bool)prepareSession {
    if (_session.source != str(_source.string) && ![self applySource:str(_source.string)]) return false;
    [self setTempo];
    return true;
}

- (void)saveSession:(id)sender {
    (void)sender;
    try {
        if (![self prepareSession]) return;
        auto* panel = [NSSavePanel savePanel];
        panel.nameFieldStringValue = @"resonant-session.rsn";
        if ([panel runModal] != NSModalResponseOK) return;
        atomic_write(Path(panel.URL.fileSystemRepresentation), encode_session(_session));
        [self message:"Session saved atomically" error:false];
    } catch (const std::exception& error) { [self message:error.what() error:true]; }
}

- (void)exportWave:(id)sender {
    (void)sender;
    if (_cancelExport) { _cancelExport->store(true); return; }
    try {
        if (![self prepareSession]) return;
        auto* panel = [NSSavePanel savePanel];
        panel.nameFieldStringValue = @"resonant-export.wav";
        if ([panel runModal] != NSModalResponseOK) return;
        const Path path(panel.URL.fileSystemRepresentation);
        const auto session = _session;
        const auto rate = [self selectedRate];
        const auto cancel = std::make_shared<std::atomic<bool>>(false);
        _cancelExport = cancel;
        _export.title = @"Cancel export";
        [self message:"Rendering through the verified offline VM" error:false];
        __weak RWorkbench* weak = self;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            std::string status;
            bool failed = false;
            try {
                status = "WAV exported with " + std::to_string(export_session(path, session, rate, cancel.get())) + " frames";
            } catch (const std::exception& error) { status = error.what(); failed = true; }
            dispatch_async(dispatch_get_main_queue(), ^{ [weak exported:status error:failed]; });
        });
    } catch (const std::exception& error) { [self message:error.what() error:true]; }
}

- (void)exported:(const std::string&)message error:(bool)error {
    _cancelExport.reset();
    _export.title = @"Export WAV";
    [self message:message error:error];
}

- (void)updateMetrics {
    if (!_engine || !_compiled) {
        _metrics.stringValue = @"AUDIO OFF   /   VERIFIED OFFLINE RENDER AVAILABLE";
        return;
    }
    const auto c = _engine->counters();
    const auto latency = _compiled->program.latency();
    _metrics.stringValue = [NSString stringWithFormat:
        @"GEN %llu/%llu   ·   %u Hz   ·   DSP %u/%u f   ·   DEVICE %.1f ms   ·   CPU %.1f%%   ·   XRUN %llu   ·   QUEUE %llu",
        static_cast<unsigned long long>(_engine->generation()), static_cast<unsigned long long>(_generation),
        _engine->sample_rate(), latency[0], latency[1], _latency * 1000,
        _lastTrace.frames != 0 ? static_cast<double>(_lastTrace.duration_ns) * _engine->sample_rate() /
                                (10000000.0 * _lastTrace.frames) : 0.0,
        static_cast<unsigned long long>(_underruns), static_cast<unsigned long long>(c.queue_full)];
}

- (void)tick:(NSTimer*)timer {
    (void)timer;
    if (!_engine) return;
    _engine->reclaim();
    Trace trace;
    while (_engine->poll(trace)) {
        _lastTrace = trace;
        [_scope pushTrace:trace rate:_engine->sample_rate()];
    }
    if (_recording) [self drainCapture];
    (void)_device->clock_anchor();
    if (++_pollCount % 10 == 0) {
        if (_midiEndpoint != 0) {
            const auto sources = apple::MidiInput::sources();
            if (std::ranges::none_of(sources, [&](const auto& source) { return source.endpoint == _midiEndpoint; })) {
                [_midiMenu selectItemAtIndex:0];
                [self selectMidi:nil];
                [self message:"MIDI source disconnected" error:true];
            }
        }
        try {
            const auto state = _device->status();
            _underruns = state.underflows;
            if (!state.active) throw std::runtime_error("Audio output stopped. Reopen or select another device.");
        } catch (const std::exception& error) {
            [self stopAudio];
            [self message:error.what() error:true];
            return;
        }
    }
    if (_midiEndpoint != 0) {
        _midiMenu.toolTip = [NSString stringWithFormat:@"MIDI dropped %llu, adjusted %llu",
                             static_cast<unsigned long long>(_midiLost),
                             static_cast<unsigned long long>(_midiAdjusted)];
    }
    [self updateMetrics];
}

- (void)startSmoke {
    try {
        [self startAudio];
        [self startCapture];
        const auto before = encode(_compiled->program);
        if (![self routeFrom:@"soft" to:@"right" port:0]) {
            throw std::runtime_error("live route failed: " + str(_message.stringValue));
        }
        if (encode(_compiled->program) == before) throw std::runtime_error("live route left bytecode unchanged");
        if ([self applySource:"x=add(x,1); out(x);"]) throw std::runtime_error("invalid graph accepted");
        [self playNote:69 on:true];
        [self playScore:nil];
        [self performSelector:@selector(finishSmoke) withObject:nil afterDelay:.65];
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::exit(1); }
}

- (void)snapshot:(const Path&)path {
    auto* view = _window.contentView;
    [view layoutSubtreeIfNeeded];
    [_window display];
    const auto rect = view.bounds;
    auto* bitmap = [view bitmapImageRepForCachingDisplayInRect:rect];
    [view cacheDisplayInRect:rect toBitmapImageRep:bitmap];
    auto* bytes = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (bytes.length == 0) throw std::runtime_error("workbench snapshot failed");
    atomic_write(path, {static_cast<const std::byte*>(bytes.bytes), bytes.length});
}

- (void)finishSmoke {
    try {
        if (!_engine || _engine->generation() != _generation || _generation < 2) {
            throw std::runtime_error("live edit did not activate");
        }
        [self playNote:69 on:false];
        [self finishCapture];
        const Path dir(str(_smoke));
        std::filesystem::create_directories(dir);
        [self snapshot:dir / "workbench.png"];
        const auto normal = _window.contentView.bounds.size;
        [_window setContentSize:_window.contentMinSize];
        [self snapshot:dir / "workbench-compact.png"];
        [_window setContentSize:normal];
        wave_output(dir / "capture.wav", _recorded, _captureRate, _captureLineage.comment());
        const auto frames = _recorded.size() / 2;
        if (!std::ranges::any_of(_recorded, [](float sample) { return std::abs(sample) > .001F; })) {
            throw std::runtime_error("recorded audio was silent");
        }
        _tempo.doubleValue = 134;
        [self setTempo];
        const auto session_path = dir / "edited.rsn";
        atomic_write(session_path, encode_session(_session));
        [self loadPath:session_path];
        const auto exported = export_session(dir / "export.wav", _session, [self selectedRate]);
        atomic_write(dir / "workbench.txt", "activated_generation=2\nrecorded_frames=" + std::to_string(frames) +
                     "\nexported_frames=" + std::to_string(exported) + "\nreopened_bpm=" +
                     std::to_string(_tempo.doubleValue) + "\n");
        [self message:"Live edit, capture, session reopen and export passed" error:false];
        if (std::getenv("RESONANT_KEEP_WINDOW") == nullptr) {
            [self stopAudio];
            [NSApp terminate:nil];
        } else {
            [self playScore:nil];
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::exit(1); }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { (void)sender; return YES; }
- (void)windowDidResignKey:(NSNotification*)notice { (void)notice; [_keys releaseNotes]; }
- (void)applicationWillTerminate:(NSNotification*)notice {
    (void)notice;
    [_timer invalidate];
    if (_midi) _midi->disconnect();
    if (_cancelExport) _cancelExport->store(true);
    [self stopAudio];
}
@end

@interface RApplication : NSApplication
@property(nonatomic) BOOL smokeMode;
@end

@implementation RApplication
- (void)reportException:(NSException*)exception {
    if (self.smokeMode) {
        std::cerr << "AppKit exception: " << str(exception.reason) << '\n';
        std::exit(1);
    }
    [super reportException:exception];
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        RApplication* app = (RApplication*)RApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSString* input = nil;
        NSString* smoke = nil;
        if (argc == 3 && std::string_view(argv[1]) == "--smoke") smoke = ns(argv[2]);
        else if (argc == 2) input = ns(argv[1]);
        app.smokeMode = smoke != nil;
        RWorkbench* delegate = [[RWorkbench alloc] initWithInput:input smoke:smoke];
        app.delegate = delegate;
        [app run];
        (void)delegate;
    }
}
