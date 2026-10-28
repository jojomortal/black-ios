// runs the spike at launch, shows the report on screen, saves it to Documents/memtest-<variant>.txt and logs it
#import <UIKit/UIKit.h>
#include <os/log.h>
#include "spike.h"

static NSString* g_text;
static NSString* g_variant;

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts {
    static char buf[16384];
    spike_run(buf, sizeof buf, 0);
    g_text = [NSString stringWithUTF8String:buf];
    g_variant = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"MemtestVariant"] ?: @"unknown";
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    [g_text writeToFile:[docs stringByAppendingPathComponent:[NSString stringWithFormat:@"memtest-%@.txt", g_variant]]
             atomically:YES encoding:NSUTF8StringEncoding error:nil];
    os_log_t log = os_log_create("com.muhammad.memtest", "spike");
    // %{public}s: the report would be <private> in the log otherwise
    os_log(log, "variant %{public}s\n%{public}s", g_variant.UTF8String, g_text.UTF8String);
    return YES;
}
- (UISceneConfiguration*)application:(UIApplication*)app configurationForConnectingSceneSession:(UISceneSession*)s options:(UISceneConnectionOptions*)o {
    UISceneConfiguration* c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:s.role];
    c.delegateClass = NSClassFromString(@"SceneDelegate");
    return c;
}
@end

// iOS 27 traps an app that has no scene lifecycle
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow* window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)opts {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    UIViewController* vc = [UIViewController new];
    UITextView* tv = [[UITextView alloc] initWithFrame:vc.view.bounds];
    tv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    tv.font = [UIFont monospacedSystemFontOfSize:9 weight:UIFontWeightRegular];
    tv.editable = NO;
    tv.text = [NSString stringWithFormat:@"variant: %@\n%@", g_variant, g_text];
    [vc.view addSubview:tv];
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class])); }
}
