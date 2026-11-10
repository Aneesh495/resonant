#pragma once

#import <AppKit/AppKit.h>

#include "resonant/engine.hpp"
#include "resonant/graph.hpp"

@interface RGraphView : NSView
@property(copy) void (^connection)(NSString* source, NSString* target, NSUInteger port);
@property(copy) void (^selection)(NSString* name);
- (void)updateGraph:(const resonant::Graph&)graph compilation:(const resonant::Compilation&)compiled;
@end

@interface RScopeView : NSView
- (void)pushTrace:(const resonant::Trace&)trace rate:(std::uint32_t)rate;
@end

@interface RKeysView : NSView
@property(copy) void (^noteEvent)(std::uint8_t note, bool on);
- (void)releaseNotes;
@end
