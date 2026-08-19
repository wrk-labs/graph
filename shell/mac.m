/* Graph.app — the desktop face of graph on macOS.
 *
 * usage: graph-shell [dir ...]
 *
 * One tab per repository, each a window in a native tab group, each backed
 * by its own `graph display --no-open` server that this process starts and
 * stops with the tab. The server is the application; a tab is a pane of
 * glass onto one of them.
 *
 * Folders arrive as arguments, from Launch Services (`open -a Graph.app
 * <dir>`, which a running instance receives as a new tab), from the "+" in
 * the tab bar or ⌘T/⌘O (a folder chooser), or from Open Recent. Started with
 * nothing, it reopens what was open last, or asks.
 *
 * State is two plain files under ~/Library/Application Support/graph:
 * `session` (the open tabs, in order) and `recent` (most recent first). */

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#define MAX_RECENT 15

/* ---- state files ---- */

static NSString *
stateDir(void)
{
	NSString *base = NSSearchPathForDirectoriesInDomains(
	    NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
	NSString *dir = [base stringByAppendingPathComponent:@"graph"];

	[[NSFileManager defaultManager] createDirectoryAtPath:dir
	    withIntermediateDirectories:YES attributes:nil error:NULL];
	return dir;
}

static NSArray<NSString *> *
readLines(NSString *name)
{
	NSString *s = [NSString stringWithContentsOfFile:
	    [stateDir() stringByAppendingPathComponent:name]
	    encoding:NSUTF8StringEncoding error:NULL];
	NSMutableArray *out = [NSMutableArray new];

	for (NSString *l in [s componentsSeparatedByString:@"\n"])
		if (l.length)
			[out addObject:l];
	return out;
}

static void
writeLines(NSString *name, NSArray<NSString *> *lines)
{
	NSString *s = [[lines componentsJoinedByString:@"\n"]
	    stringByAppendingString:lines.count ? @"\n" : @""];

	[s writeToFile:[stateDir() stringByAppendingPathComponent:name]
	    atomically:YES encoding:NSUTF8StringEncoding error:NULL];
}

static BOOL
isRepo(NSString *dir)
{
	return [[NSFileManager defaultManager] fileExistsAtPath:
	    [dir stringByAppendingPathComponent:@".graph/repository"]];
}

/* The graph binary: beside this executable in the bundle, else on $PATH. */
static NSString *
graphBinary(void)
{
	NSFileManager *fm = [NSFileManager defaultManager];
	NSString *sib = [[NSBundle mainBundle].executablePath
	    .stringByDeletingLastPathComponent stringByAppendingPathComponent:@"graph"];

	if ([fm isExecutableFileAtPath:sib])
		return sib;
	for (NSString *d in [@(getenv("PATH") ?: "") componentsSeparatedByString:@":"]) {
		NSString *c = [d stringByAppendingPathComponent:@"graph"];
		if (d.length && [fm isExecutableFileAtPath:c])
			return c;
	}
	return nil;
}

/* ---- one tab: a window, a web view, a server ---- */

@interface Tab : NSObject <NSWindowDelegate, WKNavigationDelegate, WKUIDelegate>
@property (strong) NSString *dir;
@property (strong) NSURL *home;
@property (strong) NSTask *server;
@property (strong) NSWindow *window;
@property (strong) WKWebView *web;
@end

@interface App : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property (strong) NSMutableArray<Tab *> *tabs;
@property BOOL quitting;
- (void)open:(NSString *)dir;
- (void)closed:(Tab *)tab;
@end

static App *app;

@implementation Tab

/* Start the server and wait for the line that names its URL. The server
 * prints nothing else after that, so the pipe can be left alone. */
- (BOOL)startServer:(NSString **)err
{
	NSString *graph = graphBinary();
	NSPipe *out = [NSPipe pipe], *errp = [NSPipe pipe];
	NSMutableData *buf = [NSMutableData new];
	NSFileHandle *h = out.fileHandleForReading;

	if (!graph) {
		*err = @"the graph command was not found";
		return NO;
	}
	self.server = [NSTask new];
	self.server.executableURL = [NSURL fileURLWithPath:graph];
	self.server.arguments = @[ @"display", @"--no-open",
	    @"--exit-with-parent", self.dir ];
	self.server.standardOutput = out;
	self.server.standardError = errp;
	if (![self.server launchAndReturnError:NULL]) {
		*err = @"could not start graph";
		return NO;
	}
	for (;;) {
		NSData *d = [h availableData];
		NSString *s;
		NSRange r;

		if (!d.length)
			break;	/* it exited: read why below */
		[buf appendData:d];
		s = [[NSString alloc] initWithData:buf encoding:NSUTF8StringEncoding];
		r = [s rangeOfString:@"http://"];
		if (r.location != NSNotFound &&
		    [s rangeOfString:@"\n" options:0
		    range:NSMakeRange(r.location, s.length - r.location)].location != NSNotFound) {
			NSScanner *sc = [NSScanner scannerWithString:
			    [s substringFromIndex:r.location]];
			NSString *url;
			[sc scanUpToCharactersFromSet:
			    [NSCharacterSet whitespaceAndNewlineCharacterSet] intoString:&url];
			self.home = [NSURL URLWithString:url];
			return YES;
		}
	}
	[self.server waitUntilExit];
	*err = [[[NSString alloc] initWithData:
	    [errp.fileHandleForReading readDataToEndOfFile]
	    encoding:NSUTF8StringEncoding]
	    stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
	if (!(*err).length)
		*err = @"graph exited without serving";
	self.server = nil;
	return NO;
}

- (instancetype)initWithDir:(NSString *)dir error:(NSString **)err
{
	NSRect frame = NSMakeRect(0, 0, 1180, 780);
	NSWindowStyleMask style = NSWindowStyleMaskTitled |
	    NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
	    NSWindowStyleMaskResizable;

	if (!(self = [super init]))
		return nil;
	self.dir = dir;
	if (![self startServer:err])
		return nil;

	self.window = [[NSWindow alloc] initWithContentRect:frame
	    styleMask:style backing:NSBackingStoreBuffered defer:NO];
	self.window.releasedWhenClosed = NO;	/* ARC owns it through us */
	self.window.title = dir.lastPathComponent;
	/* no represented file: the tab shows just the name, no proxy icon */
	self.window.tabbingIdentifier = @"graph";
	self.window.tabbingMode = NSWindowTabbingModePreferred;
	self.window.minSize = NSMakeSize(480, 320);
	self.window.delegate = self;
	[self.window setFrameAutosaveName:@"graph"];

	self.web = [[WKWebView alloc] initWithFrame:frame
	    configuration:[WKWebViewConfiguration new]];
	self.web.navigationDelegate = self;
	self.web.UIDelegate = self;
	self.window.contentView = self.web;
	[self.web loadRequest:[NSURLRequest requestWithURL:self.home]];
	return self;
}

- (void)stop
{
	if (self.server.running)
		[self.server terminate];
	self.server = nil;
}

- (void)windowWillClose:(NSNotification *)n
{
	[self stop];
	self.web.navigationDelegate = nil;
	self.web.UIDelegate = nil;
	self.window.delegate = nil;
	[app closed:self];
}

/* Only the server's own origin is shown here; anything else is a link out. */
- (BOOL)isHome:(NSURL *)u
{
	return [u.host isEqualToString:self.home.host] &&
	    [u.port isEqual:self.home.port];
}

- (void)webView:(WKWebView *)w
    decidePolicyForNavigationAction:(WKNavigationAction *)a
    decisionHandler:(void (^)(WKNavigationActionPolicy))decide
{
	NSURL *u = a.request.URL;

	if ([self isHome:u] || [u.scheme isEqualToString:@"about"]) {
		decide(WKNavigationActionPolicyAllow);
		return;
	}
	[[NSWorkspace sharedWorkspace] openURL:u];
	decide(WKNavigationActionPolicyCancel);
}

/* target=_blank and window.open: same rule, no second window. */
- (WKWebView *)webView:(WKWebView *)w
    createWebViewWithConfiguration:(WKWebViewConfiguration *)c
    forNavigationAction:(WKNavigationAction *)a
    windowFeatures:(WKWindowFeatures *)f
{
	NSURL *u = a.request.URL;

	if (u && [self isHome:u])
		[self.web loadRequest:a.request];
	else if (u)
		[[NSWorkspace sharedWorkspace] openURL:u];
	return nil;
}

@end

/* ---- the application: tabs, chooser, session, menus ---- */

@implementation App

- (Tab *)tabFor:(NSString *)dir
{
	for (Tab *t in self.tabs)
		if ([t.dir isEqualToString:dir])
			return t;
	return nil;
}

- (void)saveSession
{
	NSMutableArray *dirs = [NSMutableArray new];

	if (self.quitting)
		return;	/* the tabs closing now are the session to keep */
	/* in tab-bar order, so a restore puts them back as they were */
	NSArray *ws = self.tabs.firstObject.window.tabGroup.windows;
	if (!ws.count)
		ws = [self.tabs valueForKey:@"window"];
	for (NSWindow *w in ws)
		for (Tab *t in self.tabs)
			if (t.window == w)
				[dirs addObject:t.dir];
	writeLines(@"session", dirs);
}

- (void)remember:(NSString *)dir
{
	NSMutableArray *r = [readLines(@"recent") mutableCopy];

	[r removeObject:dir];
	[r insertObject:dir atIndex:0];
	while (r.count > MAX_RECENT)
		[r removeLastObject];
	writeLines(@"recent", r);
}

- (void)fail:(NSString *)dir with:(NSString *)err
{
	NSAlert *a = [NSAlert new];

	a.messageText = [NSString stringWithFormat:@"Cannot open %@",
	    dir.lastPathComponent];
	a.informativeText = err;
	[a runModal];
}

- (void)open:(NSString *)dir
{
	Tab *t;
	NSString *err = nil;
	NSWindow *key;

	dir = dir.stringByStandardizingPath;
	if ((t = [self tabFor:dir])) {
		[t.window makeKeyAndOrderFront:nil];
		return;
	}
	if (!isRepo(dir)) {
		[self fail:dir with:@"not a Graph repository"];
		return;
	}
	if (!(t = [[Tab alloc] initWithDir:dir error:&err])) {
		[self fail:dir with:err];
		return;
	}
	[self.tabs addObject:t];
	key = [NSApp keyWindow];
	if (key && [key.tabbingIdentifier isEqualToString:@"graph"])
		[key addTabbedWindow:t.window ordered:NSWindowAbove];
	else
		[t.window center];
	[t.window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];
	[self remember:dir];
	[self saveSession];
}

- (void)closed:(Tab *)tab
{
	[self.tabs removeObject:tab];
	[self saveSession];
}

/* The folder chooser. It insists on a repository, so a wrong pick is a
 * message on the sheet rather than an error afterwards. */
- (NSString *)pick
{
	NSOpenPanel *p = [NSOpenPanel openPanel];

	p.canChooseDirectories = YES;
	p.canChooseFiles = NO;
	p.allowsMultipleSelection = NO;
	p.canCreateDirectories = NO;
	p.message = @"Choose a Graph repository";
	p.prompt = @"Open";
	[NSApp activateIgnoringOtherApps:YES];
	for (;;) {
		if ([p runModal] != NSModalResponseOK)
			return nil;
		if (isRepo(p.URL.path))
			return p.URL.path;
		p.message = [NSString stringWithFormat:
		    @"%@ is not a Graph repository — choose another",
		    p.URL.path.lastPathComponent];
	}
}

- (void)pickAndOpen
{
	NSString *dir = [self pick];

	if (dir)
		[self open:dir];
}

/* The "+" in the tab bar and ⌘T land here. */
- (void)newWindowForTab:(id)sender
{
	[self pickAndOpen];
}

/* ⌘O */
- (void)openDocument:(id)sender
{
	[self pickAndOpen];
}

- (void)openRecent:(NSMenuItem *)item
{
	[self open:item.representedObject];
}

- (void)clearRecent:(id)sender
{
	writeLines(@"recent", @[]);
}

/* Open Recent is rebuilt each time it drops down. */
- (void)menuNeedsUpdate:(NSMenu *)menu
{
	NSArray *r = readLines(@"recent");

	[menu removeAllItems];
	for (NSString *dir in r) {
		NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:dir.lastPathComponent
		    action:@selector(openRecent:) keyEquivalent:@""];
		it.representedObject = dir;
		it.toolTip = dir;
		[menu addItem:it];
	}
	if (r.count)
		[menu addItem:[NSMenuItem separatorItem]];
	[menu addItemWithTitle:@"Clear Menu" action:@selector(clearRecent:)
	    keyEquivalent:@""];
}

/* Nothing open: last session if there was one, otherwise ask. */
- (void)openLastOrAsk
{
	NSArray *s = readLines(@"session");
	NSFileManager *fm = [NSFileManager defaultManager];

	for (NSString *dir in s)
		if ([fm fileExistsAtPath:dir] && isRepo(dir))
			[self open:dir];
	if (!self.tabs.count)
		[self pickAndOpen];
}

/* Folders on the command line come here; Launch Services ones below. */
- (void)applicationDidFinishLaunching:(NSNotification *)n
{
	NSArray *args = [NSProcessInfo processInfo].arguments;
	int opened = 0;

	for (NSUInteger i = 1; i < args.count; i++) {
		if ([args[i] hasPrefix:@"-"])	/* -psn_… and the like */
			continue;
		[self open:args[i]];
		opened++;
	}
	if (!opened && !self.tabs.count)
		[self openLastOrAsk];
}

- (void)application:(NSApplication *)a openURLs:(NSArray<NSURL *> *)urls
{
	for (NSURL *u in urls)
		if (u.isFileURL)
			[self open:u.path];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)a hasVisibleWindows:(BOOL)v
{
	if (!v)
		[self openLastOrAsk];
	return YES;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a
{
	return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)a
{
	self.quitting = YES;
	return NSTerminateNow;
}

- (void)applicationWillTerminate:(NSNotification *)n
{
	for (Tab *t in self.tabs)
		[t stop];
}

@end

/* Without a nib nothing gives the app a menu, and without an Edit menu the
 * web view has no ⌘C/⌘V. Build the menus that matter and nothing else. */
static void
menus(void)
{
	NSMenu *bar = [NSMenu new], *m;
	NSMenuItem *item, *recent;

	m = [NSMenu new];
	[m addItemWithTitle:@"Hide graph" action:@selector(hide:) keyEquivalent:@"h"];
	[m addItem:[NSMenuItem separatorItem]];
	[m addItemWithTitle:@"Quit graph" action:@selector(terminate:) keyEquivalent:@"q"];
	item = [NSMenuItem new];
	item.submenu = m;
	[bar addItem:item];

	m = [[NSMenu alloc] initWithTitle:@"File"];
	[m addItemWithTitle:@"New Tab" action:@selector(newWindowForTab:) keyEquivalent:@"t"];
	[m addItemWithTitle:@"Open…" action:@selector(openDocument:) keyEquivalent:@"o"];
	recent = [[NSMenuItem alloc] initWithTitle:@"Open Recent" action:NULL keyEquivalent:@""];
	recent.submenu = [[NSMenu alloc] initWithTitle:@"Open Recent"];
	recent.submenu.delegate = app;
	[m addItem:recent];
	[m addItem:[NSMenuItem separatorItem]];
	[m addItemWithTitle:@"Close Tab" action:@selector(performClose:) keyEquivalent:@"w"];
	item = [NSMenuItem new];
	item.submenu = m;
	[bar addItem:item];

	m = [[NSMenu alloc] initWithTitle:@"Edit"];
	[m addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
	[m addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
	[m addItem:[NSMenuItem separatorItem]];
	[m addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
	[m addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
	[m addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
	[m addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
	item = [NSMenuItem new];
	item.submenu = m;
	[bar addItem:item];

	m = [[NSMenu alloc] initWithTitle:@"Window"];
	[m addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
	[m addItemWithTitle:@"Show Previous Tab" action:@selector(selectPreviousTab:) keyEquivalent:@"{"];
	[m addItemWithTitle:@"Show Next Tab" action:@selector(selectNextTab:) keyEquivalent:@"}"];
	item = [NSMenuItem new];
	item.submenu = m;
	[bar addItem:item];
	NSApp.windowsMenu = m;

	NSApp.mainMenu = bar;
}

int
main(int argc, char *argv[])
{
	(void)argc; (void)argv;
	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		app = [App new];
		app.tabs = [NSMutableArray new];
		NSApp.delegate = app;
		menus();
		[NSApp run];
	}
	return 0;
}
