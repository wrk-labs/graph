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
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <util.h>

#include "term.h"

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

@interface Tab : NSObject <NSWindowDelegate, WKNavigationDelegate, WKUIDelegate,
    WKScriptMessageHandler>
@property (strong) NSString *dir;
@property (strong) NSURL *home;
@property (strong) NSTask *server;
@property (strong) NSWindow *window;
@property (strong) WKWebView *web;
- (void)shellAtSelection;
@end

/* ---- a shell tab: a window, a web view running xterm.js, a pty ---- */

@interface Term : NSObject <NSWindowDelegate, WKScriptMessageHandler,
    WKNavigationDelegate>
@property (strong) NSString *dir;
@property (strong) NSString *pending;	/* a command to run once the shell is up */
@property (strong) NSWindow *window;
@property (strong) WKWebView *web;
@property (strong) dispatch_source_t reader;
@property (strong) dispatch_source_t exited;
@property (strong) NSString *branch;	/* last branch shown in the header */
@property (strong) dispatch_source_t gitWatch;	/* fires when .git changes */
@property int master;
@property pid_t pid;
- (void)stop;
@end

@interface App : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property (strong) NSMutableArray<Tab *> *tabs;
@property (strong) NSMutableArray<Term *> *terms;
@property BOOL quitting;
- (void)open:(NSString *)dir;
- (void)closed:(Tab *)tab;
- (void)shellFor:(NSString *)dir run:(NSString *)cmd;
- (void)termClosed:(Term *)term;
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
	WKWebViewConfiguration *cfg;

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

	/* The page reaches the application through one message handler,
	 * which only a page from the server's own origin can be loaded to
	 * use (see decidePolicyForNavigationAction). */
	cfg = [WKWebViewConfiguration new];
	[cfg.userContentController addScriptMessageHandler:self name:@"graph"];
	self.web = [[WKWebView alloc] initWithFrame:frame configuration:cfg];
	self.web.navigationDelegate = self;
	self.web.UIDelegate = self;
	self.window.contentView = self.web;
	[self.web loadRequest:[NSURLRequest requestWithURL:self.home]];
	return self;
}

/* ":sh" and ":!cmd" in the page arrive here as "sh:<cmd>". */
- (void)userContentController:(WKUserContentController *)c
    didReceiveScriptMessage:(WKScriptMessage *)m
{
	NSString *s = [m.body isKindOfClass:[NSString class]] ? m.body : @"";

	if ([s hasPrefix:@"sh:"])
		[app shellFor:self.dir run:[s substringFromIndex:3]];
}

/* Where the page's selection sits, as a repository-relative folder: a
 * folder as itself, a note as the folder holding it, "" for the root. */
static NSString *const SelJS =
    @"(function(){var x=(typeof cur!=='undefined'&&cur>=0&&N[cur])?N[cur]:null;"
    @"if(!x)return'';var p=x.path||'';"
    @"if(x.type!=='dir'){var i=p.lastIndexOf('/');p=i<0?'':p.substring(0,i);}"
    @"return p;})()";

/* ⌘T. The page owns the selection, so it has to be asked for it. */
- (void)shellAtSelection
{
	NSString *root = self.dir;

	[self.web evaluateJavaScript:SelJS completionHandler:^(id v, NSError *e) {
		(void)e;
		NSString *sub = [v isKindOfClass:[NSString class]] ? v : @"";
		NSString *dir = sub.length ?
		    [root stringByAppendingPathComponent:sub] : root;
		BOOL isdir = NO;

		if (![[NSFileManager defaultManager] fileExistsAtPath:dir
		    isDirectory:&isdir] || !isdir)
			dir = root;	/* stale or renamed: the root will do */
		[app shellFor:dir run:nil];
	}];
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
	[self.web.configuration.userContentController
	    removeScriptMessageHandlerForName:@"graph"];
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

/* What a link out may be: a web page or a mail address. file:, custom
 * schemes and the rest would hand a note the power to start programs. */
static BOOL
isLinkOut(NSURL *u)
{
	NSString *s = u.scheme.lowercaseString;

	return [s isEqualToString:@"http"] || [s isEqualToString:@"https"] ||
	    [s isEqualToString:@"mailto"];
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
	if (isLinkOut(u))
		[[NSWorkspace sharedWorkspace] openURL:u];
	decide(WKNavigationActionPolicyCancel);
}

/* confirm() and alert(): without these a WKWebView answers every confirm
 * with "no", silently — so :rm and "discard changes?" could never go
 * through. A sheet on the tab's window, like any other native prompt. */
- (void)webView:(WKWebView *)w
    runJavaScriptConfirmPanelWithMessage:(NSString *)msg
    initiatedByFrame:(WKFrameInfo *)frame
    completionHandler:(void (^)(BOOL))done
{
	NSAlert *a = [NSAlert new];

	a.messageText = msg;
	[a addButtonWithTitle:@"OK"];
	[a addButtonWithTitle:@"Cancel"];
	[a beginSheetModalForWindow:self.window
	    completionHandler:^(NSModalResponse r) {
		done(r == NSAlertFirstButtonReturn);
	}];
}

- (void)webView:(WKWebView *)w
    runJavaScriptAlertPanelWithMessage:(NSString *)msg
    initiatedByFrame:(WKFrameInfo *)frame
    completionHandler:(void (^)(void))done
{
	NSAlert *a = [NSAlert new];

	a.messageText = msg;
	[a addButtonWithTitle:@"OK"];
	[a beginSheetModalForWindow:self.window
	    completionHandler:^(NSModalResponse r) { (void)r; done(); }];
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
	else if (u && isLinkOut(u))
		[[NSWorkspace sharedWorkspace] openURL:u];
	return nil;
}

@end

/* ---- the shell's sandbox ----
 *
 * A command in the shell tab may write only inside the repository and the
 * temporary directories, plus the hidden state that tools keep under the
 * home directory (~/.claude, ~/.codex, shell history, and the like). The
 * home directory's visible contents — Documents, other repositories — stay
 * readable but not writable, and the files that would let a write escape the
 * tab (shell rc files, ~/.ssh, anything on PATH, login agents) stay read
 * only. Reads are open: the shell needs its profile, its tools and their
 * libraries. Network is untouched, so git push still works.
 *
 * The list at ~/Library/Application Support/graph/writable, one path per
 * line, is applied last and can permit anything the built-in rules withheld.
 */

/* Escape a path for a Seatbelt string literal. */
static NSString *
sbLit(NSString *p)
{
	NSMutableString *o = [p mutableCopy];
	NSRange all = NSMakeRange(0, o.length);

	[o replaceOccurrencesOfString:@"\\" withString:@"\\\\" options:0 range:all];
	all = NSMakeRange(0, o.length);
	[o replaceOccurrencesOfString:@"\"" withString:@"\\\"" options:0 range:all];
	return o;
}

/* Escape a path for embedding in a Seatbelt regex. */
static NSString *
sbRe(NSString *p)
{
	NSMutableString *o = [NSMutableString string];
	NSUInteger i;

	for (i = 0; i < p.length; i++) {
		unichar c = [p characterAtIndex:i];

		if (c < 128 && strchr(".^$|()[]{}*+?\\", (int)c))
			[o appendFormat:@"\\%C", c];
		else
			[o appendFormat:@"%C", c];
	}
	return o;
}

/* The Seatbelt profile for a shell rooted at repo. */
static NSString *
sandboxProfile(NSString *repo)
{
	/* The shell writes its dotfiles under $HOME, so the profile's home rules
	 * must anchor on the same value the shell sees, not on NSHomeDirectory()
	 * (which ignores a $HOME override). In the app they are the same. */
	NSString *home = @(getenv("HOME") ?: "");
	NSString *tmp = NSTemporaryDirectory();

	if (!home.length)
		home = NSHomeDirectory();
	NSMutableString *p = [NSMutableString string];
	NSArray<NSString *> *list;

	if (tmp.length > 1 && [tmp hasSuffix:@"/"])
		tmp = [tmp substringToIndex:tmp.length - 1];

	[p appendString:@"(version 1)\n(allow default)\n(deny file-write*)\n"];

	/* Writable: the repository, the temporary directories, the tty, and the
	 * hidden state under home (~/.claude, ~/.codex, shell history, and the
	 * like). Everything visible in home — Documents, other repositories —
	 * stays readable but not writable. */
	[p appendString:@"(allow file-write*\n"];
	[p appendFormat:@"  (subpath \"%@\")\n", sbLit(repo)];
	[p appendString:@"  (subpath \"/tmp\") (subpath \"/private/tmp\")\n"];
	if (tmp.length)
		[p appendFormat:@"  (subpath \"%@\")\n", sbLit(tmp)];
	[p appendString:@"  (regex #\"^/dev/tty\") (literal \"/dev/null\")"
	    " (literal \"/dev/ptmx\")\n"];
	[p appendFormat:@"  (regex #\"^%@/\\.[^/]\"))\n", sbRe(home)];

	/* Read-only within the dotfiles: the files that would let a write
	 * outlive the tab. Last match wins, so these override the allow above. */
	[p appendString:@"(deny file-write*\n"];
	[p appendFormat:@"  (regex #\"^%@/\\.(zshenv|zprofile|zshrc|zlogin|zlogout"
	    "|bashrc|bash_profile|bash_login|profile)$\")\n", sbRe(home)];
	[p appendFormat:@"  (subpath \"%@/.ssh\")\n", sbLit(home)];
	[p appendFormat:@"  (subpath \"%@/.local/bin\"))\n", sbLit(home)];

	/* The user's own list, applied last: it can permit what the rules above
	 * withheld. Graph ships it empty. */
	list = readLines(@"writable");
	if (list.count) {
		[p appendString:@"(allow file-write*\n"];
		for (NSString *line in list) {
			NSString *e = [line stringByExpandingTildeInPath];

			[p appendFormat:@"  (subpath \"%@\")\n", sbLit(e)];
		}
		[p appendString:@")\n"];
	}
	return p;
}

/* ---- the shell tab ---- */

/* The page and the shell speak through the web view's message handler in
 * plain strings: "rs:<cols>x<rows>" (the first one starts the shell) and
 * "in:<text>" from the page; T.recv(<base64>) and T.fail() to it. The
 * server is not involved: a note's token opens notes, never a shell. */

@implementation Term

- (instancetype)initWithDir:(NSString *)dir run:(NSString *)cmd
{
	NSRect frame = NSMakeRect(0, 0, 1180, 780);
	NSWindowStyleMask style = NSWindowStyleMaskTitled |
	    NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
	    NSWindowStyleMaskResizable;
	WKWebViewConfiguration *cfg = [WKWebViewConfiguration new];

	if (!(self = [super init]))
		return nil;
	self.dir = dir;
	self.pending = cmd.length ? cmd : nil;
	self.master = -1;

	[cfg.userContentController addScriptMessageHandler:self name:@"term"];
	self.window = [[NSWindow alloc] initWithContentRect:frame
	    styleMask:style backing:NSBackingStoreBuffered defer:NO];
	self.window.releasedWhenClosed = NO;
	self.window.title = [NSString stringWithFormat:@"❯ %@",
	    dir.lastPathComponent];
	self.window.tabbingIdentifier = @"graph";
	self.window.tabbingMode = NSWindowTabbingModePreferred;
	self.window.minSize = NSMakeSize(480, 320);
	self.window.delegate = self;
	[self.window setFrameAutosaveName:@"graph"];

	self.web = [[WKWebView alloc] initWithFrame:frame configuration:cfg];
	self.web.navigationDelegate = self;
	self.window.contentView = self.web;
	[self.web loadHTMLString:@((const char *)term_html) baseURL:nil];
	return self;
}

/* The repository's branch, for the header. A quick read; nil if the path is
 * not a checkout or git is unavailable. */
- (NSString *)gitBranch
{
	NSTask *t = [NSTask new];
	NSPipe *out = [NSPipe pipe];
	NSData *d;
	NSString *s;

	t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/git"];
	t.arguments = @[ @"-C", self.dir, @"symbolic-ref", @"--short", @"HEAD" ];
	t.standardOutput = out;
	t.standardError = [NSPipe pipe];
	if (![t launchAndReturnError:NULL])
		return nil;
	d = [out.fileHandleForReading readDataToEndOfFile];
	[t waitUntilExit];
	if (t.terminationStatus != 0)
		return nil;
	s = [[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding]
	    stringByTrimmingCharactersInSet:
	    [NSCharacterSet whitespaceAndNewlineCharacterSet]];
	return s.length ? s : nil;
}

/* Fill the header once the page is up: what repository this is, and that the
 * shell is confined. The writable/read-only summary mirrors sandboxProfile. */
/* Fill the header with what this repository is and how it is confined. */
- (void)pushInfoWithBranch:(NSString *)branch
{
	NSString *home = @(getenv("HOME") ?: "");
	NSString *path = self.dir;
	NSMutableDictionary *o = [@{
		@"name": self.dir.lastPathComponent ?: @"",
		@"sandboxed": @(access("/usr/bin/sandbox-exec", X_OK) == 0),
		@"writable": @[ @"repo", @"/tmp", @"~/dotfiles" ],
		@"readonly": @[ @"your files", @"~/.ssh", @"shell config" ],
	} mutableCopy];
	NSData *j;

	self.branch = branch;
	if (home.length && [path hasPrefix:home])
		path = [@"~" stringByAppendingString:
		    [path substringFromIndex:home.length]];
	o[@"path"] = path;
	if (branch)
		o[@"branch"] = branch;

	j = [NSJSONSerialization dataWithJSONObject:o options:0 error:NULL];
	[self.web evaluateJavaScript:[NSString stringWithFormat:@"T.info(%@)",
	    [[NSString alloc] initWithData:j encoding:NSUTF8StringEncoding]]
	    completionHandler:nil];
}

/* Re-read the branch and update the header only if it actually changed. */
- (void)refreshBranch
{
	NSString *b = [self gitBranch];

	if (b == self.branch || [b isEqualToString:self.branch])
		return;
	[self pushInfoWithBranch:b];
}

/* The actual git directory for the repository, which may sit above it (a repo
 * nested in another checkout) rather than at <repo>/.git. */
- (NSString *)gitDir
{
	NSTask *t = [NSTask new];
	NSPipe *out = [NSPipe pipe];
	NSData *d;
	NSString *s;

	t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/git"];
	t.arguments = @[ @"-C", self.dir, @"rev-parse", @"--absolute-git-dir" ];
	t.standardOutput = out;
	t.standardError = [NSPipe pipe];
	if (![t launchAndReturnError:NULL])
		return nil;
	d = [out.fileHandleForReading readDataToEndOfFile];
	[t waitUntilExit];
	if (t.terminationStatus != 0)
		return nil;
	s = [[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding]
	    stringByTrimmingCharactersInSet:
	    [NSCharacterSet whitespaceAndNewlineCharacterSet]];
	return s.length ? s : nil;
}

/* Watch the git directory for changes, so switching branches updates the
 * header at once. Event-driven: nothing runs unless git writes something.
 * The directory is watched rather than HEAD itself, since git replaces HEAD
 * by rename and a watch on the file would go stale after one switch. */
- (void)watchGit
{
	NSString *g = [self gitDir];
	int fd = g ? open(g.fileSystemRepresentation, O_EVTONLY) : -1;
	__weak Term *me = self;

	if (fd < 0)
		return;		/* not a checkout; the header just stays put */
	self.gitWatch = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE,
	    (uintptr_t)fd, DISPATCH_VNODE_WRITE | DISPATCH_VNODE_DELETE |
	    DISPATCH_VNODE_RENAME, dispatch_get_main_queue());
	dispatch_source_set_event_handler(self.gitWatch, ^{ [me refreshBranch]; });
	dispatch_source_set_cancel_handler(self.gitWatch, ^{ close(fd); });
	dispatch_resume(self.gitWatch);
}

- (void)webView:(WKWebView *)w didFinishNavigation:(WKNavigation *)nav
{
	(void)w; (void)nav;
	[self pushInfoWithBranch:[self gitBranch]];
	[self watchGit];
	/* Also refresh when this tab comes back to the front: a branch changed
	 * in another terminal shows the moment you look at the shell again. */
	[[NSNotificationCenter defaultCenter] addObserver:self
	    selector:@selector(refreshBranch)
	    name:NSWindowDidBecomeKeyNotification object:self.window];
}

- (void)send:(NSString *)s
{
	const char *p = s.UTF8String;
	size_t left = strlen(p);
	ssize_t n;

	while (left > 0 && self.master >= 0) {
		if ((n = write(self.master, p, left)) <= 0)
			break;
		p += n;
		left -= (size_t)n;
	}
}

/* Whatever the shell wrote, handed to the page as it arrives. */
- (void)pump
{
	char buf[65536];
	ssize_t n = read(self.master, buf, sizeof(buf));
	NSString *js;

	if (n <= 0) {	/* the last process on the tty is gone (EIO here) */
		dispatch_source_cancel(self.reader);	/* closes master */
		self.reader = nil;
		self.master = -1;
		return;
	}
	js = [NSString stringWithFormat:@"T.recv(\"%@\")",
	    [[NSData dataWithBytes:buf length:(NSUInteger)n]
	    base64EncodedStringWithOptions:0]];
	[self.web evaluateJavaScript:js completionHandler:nil];
}

- (BOOL)spawnCols:(int)cols rows:(int)rows
{
	struct winsize ws;
	const char *shell = getenv("SHELL");
	/* Taken before the fork: the child must not touch the Objective-C
	 * runtime, which is not fork-safe. */
	const char *cwd = self.dir.fileSystemRepresentation;
	const char *profile = sandboxProfile(self.dir).UTF8String;
	int sandbox = access("/usr/bin/sandbox-exec", X_OK) == 0;
	__weak Term *me = self;
	pid_t pid;
	int fd;

	memset(&ws, 0, sizeof(ws));
	ws.ws_col = (unsigned short)cols;
	ws.ws_row = (unsigned short)rows;
	if (!shell || !*shell)
		shell = "/bin/zsh";
	if ((pid = forkpty(&fd, NULL, NULL, &ws)) < 0)
		return NO;
	if (pid == 0) {
		/* From the Dock the environment is bare: no locale, so the
		 * shell would take the terminal for ASCII. A login shell
		 * for the PATH the user's profile sets. */
		if (!getenv("LANG"))
			setenv("LANG", "en_US.UTF-8", 1);
		setenv("TERM", "xterm-256color", 1);
		setenv("TERM_PROGRAM", "graph", 1);
		setenv("SHELL_SESSIONS_DISABLE", "1", 1);
		if (chdir(cwd) < 0)
			_exit(127);
		/* sandbox-exec applies the profile, then execs the shell into
		 * it. If it is present the shell is never started without it:
		 * a rejected profile fails closed rather than dropping the
		 * confinement. */
		if (sandbox) {
			execl("/usr/bin/sandbox-exec", "sandbox-exec", "-p",
			    profile, shell, "-l", (char *)NULL);
			_exit(127);
		}
		execl(shell, shell, "-l", (char *)NULL);
		_exit(127);
	}
	self.pid = pid;
	self.master = fd;

	self.reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)fd, 0, dispatch_get_main_queue());
	dispatch_source_set_event_handler(self.reader, ^{ [me pump]; });
	dispatch_source_set_cancel_handler(self.reader, ^{ close(fd); });
	dispatch_resume(self.reader);

	/* The shell's exit closes the tab, so `exit` means what it says. */
	self.exited = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC,
	    (uintptr_t)pid, DISPATCH_PROC_EXIT, dispatch_get_main_queue());
	dispatch_source_set_event_handler(self.exited, ^{
		waitpid(pid, NULL, WNOHANG);
		me.pid = 0;
		[me.window close];
	});
	dispatch_resume(self.exited);

	if (self.pending) {
		[self send:[self.pending stringByAppendingString:@"\n"]];
		self.pending = nil;
	}
	return YES;
}

- (void)userContentController:(WKUserContentController *)c
    didReceiveScriptMessage:(WKScriptMessage *)m
{
	NSString *s = [m.body isKindOfClass:[NSString class]] ? m.body : @"";
	struct winsize ws;
	int cols, rows;

	if ([s hasPrefix:@"in:"]) {
		[self send:[s substringFromIndex:3]];
		return;
	}
	if (![s hasPrefix:@"rs:"] ||
	    sscanf(s.UTF8String + 3, "%dx%d", &cols, &rows) != 2 ||
	    cols <= 0 || rows <= 0)
		return;
	if (self.master < 0) {
		if (![self spawnCols:cols rows:rows])
			[self.web evaluateJavaScript:@"T.fail()" completionHandler:nil];
		return;
	}
	memset(&ws, 0, sizeof(ws));
	ws.ws_col = (unsigned short)cols;
	ws.ws_row = (unsigned short)rows;
	ioctl(self.master, TIOCSWINSZ, &ws);
}

/* Hang up on the shell and reap it. A shell that will not go on SIGHUP
 * — something in it is trapping the signal — is given a moment, then
 * killed, so closing a tab never hangs the window. */
- (void)stop
{
	pid_t pid = self.pid;
	int i;

	if (self.gitWatch) {
		dispatch_source_cancel(self.gitWatch);	/* closes the .git fd */
		self.gitWatch = nil;
	}
	if (self.reader) {
		dispatch_source_cancel(self.reader);	/* closes master */
		self.reader = nil;
	} else if (self.master >= 0) {
		close(self.master);
	}
	self.master = -1;
	if (self.exited) {
		dispatch_source_cancel(self.exited);
		self.exited = nil;
	}
	if (pid <= 0)
		return;
	self.pid = 0;
	kill(pid, SIGHUP);
	for (i = 0; i < 20; i++) {
		if (waitpid(pid, NULL, WNOHANG) != 0)
			return;
		usleep(10000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

- (void)windowWillClose:(NSNotification *)n
{
	[self stop];
	[[NSNotificationCenter defaultCenter] removeObserver:self];
	[self.web.configuration.userContentController
	    removeScriptMessageHandlerForName:@"term"];
	self.web.navigationDelegate = nil;
	self.window.delegate = nil;
	[app termClosed:self];
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

/* A shell tab for a repository. ":sh" always opens a new one; ":!cmd" with
 * a shell already open for that repository runs there instead, as a
 * second prompt would only get in the way of the first. Shell tabs are
 * not part of the session: a shell is a moment, a repository is not. */
- (void)shellFor:(NSString *)dir run:(NSString *)cmd
{
	NSWindow *key = [NSApp keyWindow];
	Term *t;

	if (cmd.length) {
		for (t in self.terms.reverseObjectEnumerator) {
			if (![t.dir isEqualToString:dir] || t.master < 0)
				continue;
			[t send:[cmd stringByAppendingString:@"\n"]];
			[t.window makeKeyAndOrderFront:nil];
			return;
		}
	}
	t = [[Term alloc] initWithDir:dir run:cmd];
	[self.terms addObject:t];
	if (key && [key.tabbingIdentifier isEqualToString:@"graph"])
		[key addTabbedWindow:t.window ordered:NSWindowAbove];
	else
		[t.window center];
	[t.window makeKeyAndOrderFront:nil];
}

- (void)termClosed:(Term *)term
{
	[self.terms removeObject:term];
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

/* The "+" in the tab bar. */
- (void)newWindowForTab:(id)sender
{
	[self pickAndOpen];
}

/* ⌘T: a shell where you already are — the selected folder of a display
 * tab, the same folder again for a shell tab, as Terminal does. */
- (void)newShellTab:(id)sender
{
	NSWindow *key = [NSApp keyWindow];

	for (Term *t in self.terms)
		if (t.window == key) {
			[self shellFor:t.dir run:nil];
			return;
		}
	for (Tab *t in self.tabs)
		if (t.window == key) {
			[t shellAtSelection];
			return;
		}
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
	for (Term *t in self.terms)
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
	[m addItemWithTitle:@"New Shell Tab" action:@selector(newShellTab:) keyEquivalent:@"t"];
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

/* ⌘1…⌘9 show the tab in that position, as in Safari and Terminal. Caught
 * before dispatch rather than listed in a menu: nine "Tab N" items would say
 * nothing the tab bar does not already show. */
static void
tabKeys(void)
{
	[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
	    handler:^NSEvent *(NSEvent *e) {
		NSEventModifierFlags mods = e.modifierFlags &
		    NSEventModifierFlagDeviceIndependentFlagsMask;
		NSString *c = e.charactersIgnoringModifiers;
		NSWindowTabGroup *group;
		NSUInteger i;

		if (mods != NSEventModifierFlagCommand || c.length != 1 ||
		    [c characterAtIndex:0] < '1' || [c characterAtIndex:0] > '9')
			return e;
		i = (NSUInteger)([c characterAtIndex:0] - '1');
		group = NSApp.keyWindow.tabGroup;
		if (group && i < group.windows.count)
			group.selectedWindow = group.windows[i];
		return nil;
	}];
}

int
main(int argc, char *argv[])
{
	/* Test hook: run a command under the exact sandbox a shell tab for
	 * <repo> would get, then exit. Used by test/sandbox_test.sh to check
	 * the policy against the real profile; not part of the UI. */
	if (argc >= 4 && !strcmp(argv[1], "--sandbox-run")) {
		@autoreleasepool {
			const char *profile =
			    sandboxProfile(@(argv[2])).UTF8String;
			int n = argc - 3, i;
			char **a = calloc((size_t)n + 4, sizeof(*a));

			a[0] = (char *)"sandbox-exec";
			a[1] = (char *)"-p";
			a[2] = (char *)profile;
			for (i = 0; i < n; i++)
				a[3 + i] = argv[3 + i];
			a[3 + n] = NULL;
			execv("/usr/bin/sandbox-exec", a);
			perror("sandbox-exec");
			return 127;
		}
	}

	(void)argc; (void)argv;
	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		app = [App new];
		app.tabs = [NSMutableArray new];
		app.terms = [NSMutableArray new];
		NSApp.delegate = app;
		menus();
		tabKeys();
		[NSApp run];
	}
	return 0;
}
