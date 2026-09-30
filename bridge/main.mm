// truSDX Bridge: menu bar app (default) or terminal daemon (--headless) that
// connects a (tr)uSDX's USB serial audio stream to the "truSDX" Core Audio
// device and exposes its CAT port as a pseudo-terminal.

#include "Bridge.hpp"
#include "Log.hpp"

#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

std::atomic<bool> gStop{false};

void OnSignal(int)
{
    gStop = true;
}

void Usage()
{
    std::fprintf(stderr,
        "usage: TruSDXBridge [options]\n"
        "  --headless         run in the terminal instead of the menu bar\n"
        "  --port PATH        radio serial port (default: first /dev/cu.wchusbserial*)\n"
        "  --cat PATH         CAT pseudo-terminal link for WSJT-X (default /tmp/trusdx-cat)\n"
        "  --speaker          keep the radio's speaker on while streaming\n"
        "  --tx-gain X        scale transmit audio (default 1.0)\n"
        "  --tx-timeout SEC   force RX after this long keyed (default 180)\n"
        "  --rx-rate HZ       nominal radio receive sample rate (default 7812.5)\n"
        "  -v, --verbose      log CAT traffic and stats\n");
}

// Returns false (after printing usage) on a bad argument.
bool ParseArgs(int argc, char** argv, trusdx::Options* options, bool* headless)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                Usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--headless") {
            *headless = true;
        } else if (arg == "--port") {
            options->port = value();
        } else if (arg == "--cat") {
            options->catLink = value();
        } else if (arg == "--speaker") {
            options->speaker = true;
        } else if (arg == "--tx-gain") {
            options->txGain = std::strtof(value(), nullptr);
        } else if (arg == "--tx-timeout") {
            options->txTimeoutSec = std::atoi(value());
        } else if (arg == "--rx-rate") {
            options->rxRate = std::strtod(value(), nullptr);
        } else if (arg == "-v" || arg == "--verbose") {
            options->verbose = true;
        } else if (arg.rfind("-psn_", 0) == 0) {
            // Finder/LaunchServices process serial number; ignore.
        } else {
            Usage();
            return false;
        }
    }
    return true;
}

int RunHeadless(const trusdx::Options& options)
{
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGHUP, OnSignal);
    trusdx::Bridge bridge(options);
    return bridge.Run(gStop);
}

// Runs a Bridge on a background thread so it can be started and stopped from the UI.
class BridgeRunner {
public:
    ~BridgeRunner() { Stop(); }

    void Start(const trusdx::Options& options)
    {
        Stop();
        stop_ = false;
        bridge_ = std::make_unique<trusdx::Bridge>(options);
        thread_ = std::thread([this] { bridge_->Run(stop_); });
    }

    void Stop()
    {
        if (!bridge_) {
            return;
        }
        stop_ = true;
        thread_.join(); // unkeys the radio and restores its speaker on the way out
        bridge_.reset();
    }

    bool Running() const { return bridge_ != nullptr; }
    trusdx::BridgeStatus Status() { return bridge_ ? bridge_->Status() : trusdx::BridgeStatus{}; }

private:
    std::unique_ptr<trusdx::Bridge> bridge_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

NSString* const kAutoStartKey = @"StartBridgeAtLaunch";
NSString* const kSpeakerKey = @"RadioSpeakerOn";

NSString* LogPath()
{
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/trusdx-bridge.log"];
}

} // namespace

@interface AppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate>
@end

@implementation AppDelegate {
    trusdx::Options _options;
    BridgeRunner _runner;
    NSStatusItem* _item;
    NSMenuItem* _radioLine;
    NSMenuItem* _audioLine;
    NSMenuItem* _toggle;
    NSMenuItem* _speaker;
    NSMenuItem* _login;
    NSTimer* _timer;
}

- (instancetype)initWithOptions:(const trusdx::Options&)options
{
    if ((self = [super init])) {
        _options = options;
    }
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note
{
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults registerDefaults:@{kAutoStartKey : @YES, kSpeakerKey : @NO}];

    _item = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];

    NSMenu* menu = [[NSMenu alloc] init];
    menu.delegate = self;
    menu.autoenablesItems = NO;

    _radioLine = [menu addItemWithTitle:@"" action:nil keyEquivalent:@""];
    _radioLine.enabled = NO;
    _audioLine = [menu addItemWithTitle:@"" action:nil keyEquivalent:@""];
    _audioLine.enabled = NO;
    NSMenuItem* catLine = [menu addItemWithTitle:[NSString stringWithFormat:@"CAT port: %s",
                                                  _options.catLink.c_str()]
                                          action:nil
                                   keyEquivalent:@""];
    catLine.enabled = NO;
    [menu addItem:NSMenuItem.separatorItem];

    _toggle = [menu addItemWithTitle:@"" action:@selector(toggleBridge:) keyEquivalent:@"s"];
    _toggle.target = self;
    _speaker = [menu addItemWithTitle:@"Radio Speaker On"
                               action:@selector(toggleSpeaker:)
                        keyEquivalent:@""];
    _speaker.target = self;
    _login = [menu addItemWithTitle:@"Open at Login" action:@selector(toggleLogin:) keyEquivalent:@""];
    _login.target = self;
    NSMenuItem* log = [menu addItemWithTitle:@"Show Log" action:@selector(showLog:) keyEquivalent:@"l"];
    log.target = self;
    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem* quit = [menu addItemWithTitle:@"Quit truSDX Bridge"
                                       action:@selector(terminate:)
                                keyEquivalent:@"q"];
    quit.target = NSApp;
    _item.menu = menu;

    if ([defaults boolForKey:kAutoStartKey]) {
        [self startBridge];
    }
    [self refresh];
    // Common modes keep the status live while the menu is open.
    _timer = [NSTimer timerWithTimeInterval:0.5
                                     target:self
                                   selector:@selector(refresh)
                                   userInfo:nil
                                    repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (void)applicationWillTerminate:(NSNotification*)note
{
    _runner.Stop();
}

- (void)startBridge
{
    trusdx::Options options = _options;
    options.speaker = options.speaker || [NSUserDefaults.standardUserDefaults boolForKey:kSpeakerKey];
    _runner.Start(options);
}

- (void)toggleBridge:(id)sender
{
    const bool start = !_runner.Running();
    if (start) {
        [self startBridge];
    } else {
        _runner.Stop();
    }
    // Remember the choice so the next launch starts (or doesn't) the same way.
    [NSUserDefaults.standardUserDefaults setBool:start forKey:kAutoStartKey];
    [self refresh];
}

- (void)toggleSpeaker:(id)sender
{
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults setBool:![defaults boolForKey:kSpeakerKey] forKey:kSpeakerKey];
    if (_runner.Running()) {
        [self startBridge]; // the streaming mode is chosen when the radio connects
    }
    [self refresh];
}

- (void)toggleLogin:(id)sender
{
    SMAppService* service = SMAppService.mainAppService;
    NSError* error = nil;
    const BOOL ok = service.status == SMAppServiceStatusEnabled
        ? [service unregisterAndReturnError:&error]
        : [service registerAndReturnError:&error];
    if (!ok) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"Couldn't change Open at Login";
        alert.informativeText = error.localizedDescription ?: @"Unknown error";
        [alert runModal];
    }
    [self refresh];
}

- (void)showLog:(id)sender
{
    [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:LogPath()]];
}

- (void)menuWillOpen:(NSMenu*)menu
{
    [self refresh];
}

- (void)refresh
{
    const bool running = _runner.Running();
    const trusdx::BridgeStatus status = _runner.Status();

    NSString* symbol = @"antenna.radiowaves.left.and.right";
    NSColor* tint = nil;
    if (!running) {
        symbol = @"antenna.radiowaves.left.and.right.slash";
    } else if (status.transmitting) {
        tint = NSColor.systemRedColor;
    } else if (!status.radioConnected || !status.audioConnected) {
        tint = NSColor.systemOrangeColor;
    }
    NSImage* image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:@"truSDX Bridge"];
    [image setTemplate:YES]; // `template` is a C++ keyword, so no dot syntax
    _item.button.image = image;
    _item.button.contentTintColor = tint;
    _item.button.alphaValue = running ? 1.0 : 0.6;

    if (!running) {
        _radioLine.title = @"Bridge stopped";
        _audioLine.hidden = YES;
    } else if (!status.error.empty()) {
        _radioLine.title = [NSString stringWithFormat:@"Error: %s", status.error.c_str()];
        _audioLine.hidden = YES;
    } else {
        NSString* radio = @"Radio: waiting for USB…";
        if (status.radioConnected) {
            radio = status.frequencyHz
                ? [NSString stringWithFormat:@"Radio: %.3f MHz", status.frequencyHz / 1e6]
                : @"Radio: connected";
            if (status.transmitting) {
                radio = [radio stringByAppendingString:@" — TRANSMITTING"];
            }
        }
        _radioLine.title = radio;
        _audioLine.title = status.audioConnected ? @"Audio device: truSDX"
                                                 : @"Audio device: driver not found";
        _audioLine.hidden = NO;
    }

    _toggle.title = running ? @"Stop Bridge" : @"Start Bridge";
    _speaker.state = [NSUserDefaults.standardUserDefaults boolForKey:kSpeakerKey]
        ? NSControlStateValueOn
        : NSControlStateValueOff;
    _login.state = SMAppService.mainAppService.status == SMAppServiceStatusEnabled
        ? NSControlStateValueOn
        : NSControlStateValueOff;
}

@end

int main(int argc, char** argv)
{
    trusdx::Options options;
    bool headless = false;
    if (!ParseArgs(argc, argv, &options, &headless)) {
        return 2;
    }
    std::signal(SIGPIPE, SIG_IGN);

    if (headless) {
        return RunHeadless(options);
    }

    // Launched from Finder or at login there is no terminal; keep a log file instead.
    if (!isatty(STDERR_FILENO)) {
        std::freopen(LogPath().fileSystemRepresentation, "a", stderr);
        setvbuf(stderr, nullptr, _IOLBF, 0); // freopen made it fully buffered
    }

    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyAccessory;
        AppDelegate* delegate = [[AppDelegate alloc] initWithOptions:options];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
