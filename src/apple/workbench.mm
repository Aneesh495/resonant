#import "views.hpp"

#include "resonant/device.hpp"
#include "resonant/session.hpp"
#include "resonant/wave.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

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
    auto* item = [NSButton buttonWithTitle:title target:target action:action];
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
    view.textContainerInset = NSMakeSize(10, 9);
    view.verticallyResizable = YES;
    view.horizontallyResizable = NO;
    view.textContainer.widthTracksTextView = YES;
    view.autoresizingMask = NSViewWidthSizable;
    view.richText = NO;
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

void wave_output(const Path& path, const std::vector<float>& samples, std::uint32_t rate) {
    const std::vector<double> pcm(samples.begin(), samples.end());
    atomic_write(path, encode_wave(pcm, {Encoding::pcm24, rate, 2, 0}));
}

std::uint64_t export_session(const Path& path, const Session& session, std::uint32_t rate,
                             const std::atomic<bool>* cancel = nullptr) {
    const auto c = compile(session.source);
    const auto timeline = schedule(session, rate);
    const auto result = render_offline(c.program, timeline, rate, 128, Execution::block, cancel);
    if (cancel && cancel->load()) throw std::runtime_error("export cancelled");
    wave_output(path, result.samples, rate);
    return timeline.frames;
}
}

@interface RWorkbench : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTextViewDelegate>
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
    RGraphView* _graph;
    RScopeView* _scope;
    RKeysView* _keys;
    NSString* _input;
    NSString* _smoke;
    NSTimer* _timer;
    Session _session;
    std::unique_ptr<Compilation> _compiled;
    std::unique_ptr<Engine> _engine;
    std::unique_ptr<Device> _device;
    std::vector<int> _devices;
    Timeline _timeline;
    std::size_t _scoreIndex;
    std::uint64_t _scoreBase;
    std::uint64_t _lastEvent;
    std::uint64_t _captureNext;
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
    _rate = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_rate addItemsWithTitles:@[@"44100", @"48000", @"96000"]];
    [_rate selectItemWithTitle:@"48000"];
    _rate.target = self;
    _rate.action = @selector(reconfigure:);
    [_rate setAccessibilityLabel:@"Sample rate"];
    _output = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_output addItemWithTitle:@"Default output"];
    _devices.push_back(-1);
    _output.target = self;
    _output.action = @selector(reconfigure:);
    [_output setAccessibilityLabel:@"Audio output device"];
    _level = [NSSlider sliderWithValue:0 minValue:0 maxValue:.2 target:self action:@selector(monitor:)];
    [_level setAccessibilityLabel:@"Monitor gain"];
    minimum(_level, 80);
    _tempo = [NSTextField textFieldWithString:@"120"];
    _tempo.target = self;
    _tempo.action = @selector(tempo:);
    [_tempo setAccessibilityLabel:@"Initial tempo in beats per minute"];
    minimum(_tempo, 57);
    _message = text(@"Ready");
    _message.lineBreakMode = NSLineBreakByTruncatingTail;
    _metrics = text(@"AUDIO OFF", 11, true);
    _metrics.lineBreakMode = NSLineBreakByTruncatingTail;

    _from = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _to = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _port = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _parameter = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
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
    minimum(brand, 120);
    auto* upper = row(@[brand, button(@"Open…", self, @selector(openSession:), @"o"),
        button(@"Save…", self, @selector(saveSession:), @"s"),
        button(@"Apply patch", self, @selector(applyPatch:), @"\r"), _audio, _rate, _output, text(@"Monitor"), _level]);
    auto* transport = row(@[button(@"Play score", self, @selector(playScore:)),
        button(@"Stop", self, @selector(stopScore:)), text(@"BPM"), _tempo,
        _record, _export, button(@"Focus keys", self, @selector(focusKeys:))]);
    auto* route = row(@[text(@"ROUTE", 10, true), _from, text(@"→"), _to, _port,
        button(@"Connect", self, @selector(connect:)), text(@"CONTROL", 10, true), _parameter, _parameterValue]);

    auto* tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0,0,480,340)];
    auto* patchTab = [[NSTabViewItem alloc] initWithIdentifier:@"source"];
    patchTab.label = @"Patch source";
    patchTab.view = scroll(_source);
    auto* codeTab = [[NSTabViewItem alloc] initWithIdentifier:@"code"];
    codeTab.label = @"Compiled schedule";
    codeTab.view = scroll(_listing);
    [tabs addTabViewItem:patchTab];
    [tabs addTabViewItem:codeTab];

    auto* split = [[NSSplitView alloc] initWithFrame:NSMakeRect(0,0,1000,360)];
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    [split addArrangedSubview:scroll(_graph)];
    [split addArrangedSubview:tabs];

    NSView* base = _window.contentView;
    auto* layout = [NSStackView stackViewWithViews:@[upper, transport, route, split, _scope, _keys, _metrics, _message]];
    layout.orientation = NSUserInterfaceLayoutOrientationVertical;
    layout.alignment = NSLayoutAttributeLeading;
    layout.spacing = 7;
    layout.translatesAutoresizingMaskIntoConstraints = NO;
    [base addSubview:layout];
    [NSLayoutConstraint activateConstraints:@[
        [layout.leadingAnchor constraintEqualToAnchor:base.leadingAnchor constant:13],
        [layout.trailingAnchor constraintEqualToAnchor:base.trailingAnchor constant:-13],
        [layout.topAnchor constraintEqualToAnchor:base.topAnchor constant:11],
        [layout.bottomAnchor constraintEqualToAnchor:base.bottomAnchor constant:-11],
        [split.widthAnchor constraintEqualToAnchor:layout.widthAnchor],
        [split.heightAnchor constraintGreaterThanOrEqualToConstant:200],
        [_scope.widthAnchor constraintEqualToAnchor:layout.widthAnchor],
        [_scope.heightAnchor constraintEqualToConstant:100],
        [_keys.widthAnchor constraintEqualToAnchor:layout.widthAnchor],
        [_keys.heightAnchor constraintEqualToConstant:29],
        [_metrics.widthAnchor constraintEqualToAnchor:layout.widthAnchor],
        [_message.widthAnchor constraintEqualToAnchor:layout.widthAnchor],
    ]];
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
        const auto span = [name isEqualToString:@"out"] ? graph.output_span :
                          found != graph.expressions.end() ? found->span : SourceSpan{};
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
    _device->open(*next, {_devices.at(index), 128});
    _engine = std::move(next);
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
    if (_engine) _engine->cancel_events();
    _lastEvent = 0;
}

- (void)startCapture {
    if (!_engine) throw std::runtime_error("start audio before recording");
    CaptureBlock block;
    while (_engine->poll_capture(block)) {}
    _recorded.clear();
    _captureNext = 0;
    _captureOverflow = _engine->capture_overflow();
    _captureFailed = false;
    _recording = true;
    _engine->capture(true);
    _record.title = @"Stop & save";
    [self message:"Recording the stereo VM mix" error:false];
}

- (void)drainCapture {
    CaptureBlock block;
    const auto capacity = static_cast<std::size_t>(_engine->sample_rate()) * 2 * 60;
    while (_engine->poll_capture(block)) {
        if (_captureNext != 0 && _captureNext != block.first_frame) _captureFailed = true;
        const auto count = static_cast<std::size_t>(2 * block.frames);
        if (count > capacity - _recorded.size()) {
            _engine->capture(false);
            [self message:"Recording reached the 60-second buffer limit" error:false];
            continue;
        }
        _recorded.insert(_recorded.end(), block.interleaved.begin(),
                         block.interleaved.begin() + static_cast<std::ptrdiff_t>(count));
        _captureNext = block.first_frame + block.frames;
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
        wave_output(Path(panel.URL.fileSystemRepresentation), _recorded, [self selectedRate]);
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
    if (_scorePlaying && !_engine->cancellation_pending()) {
        const auto cursor = _engine->clock();
        const auto horizon = cursor + _engine->sample_rate() / 20;
        while (_scoreIndex < _timeline.events.size()) {
            auto event = _timeline.events[_scoreIndex];
            event.frame += _scoreBase;
            if (event.frame >= horizon) break;
            event.frame = std::max(event.frame, _lastEvent);
            if (_engine->enqueue(event) != Enqueue::accepted) {
                [self message:"Score transport stopped after event queue rejection" error:true];
                [self stopScore:nil];
                break;
            }
            _lastEvent = event.frame;
            ++_scoreIndex;
        }
        if (cursor >= _scoreBase + _timeline.frames) [self stopScore:nil];
    }
    if (++_pollCount % 10 == 0) {
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
    [self updateMetrics];
}

- (void)startSmoke {
    try {
        [self startAudio];
        [self startCapture];
        const auto before = encode(_compiled->program);
        if (![self routeFrom:@"soft" to:@"right" port:0] || encode(_compiled->program) == before) {
            throw std::runtime_error("live route failed");
        }
        if ([self applySource:"x=add(x,1); out(x);"]) throw std::runtime_error("invalid graph accepted");
        [self playNote:69 on:true];
        [self playScore:nil];
        [self performSelector:@selector(finishSmoke) withObject:nil afterDelay:.65];
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::exit(1); }
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
        wave_output(dir / "capture.wav", _recorded, [self selectedRate]);
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
    if (_cancelExport) _cancelExport->store(true);
    [self stopAudio];
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        auto* app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSString* input = nil;
        NSString* smoke = nil;
        if (argc == 3 && std::string_view(argv[1]) == "--smoke") smoke = ns(argv[2]);
        else if (argc == 2) input = ns(argv[1]);
        RWorkbench* delegate = [[RWorkbench alloc] initWithInput:input smoke:smoke];
        app.delegate = delegate;
        [app run];
        (void)delegate;
    }
}
