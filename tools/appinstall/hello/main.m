#import <UIKit/UIKit.h>
@interface AD : UIResponder <UIApplicationDelegate>
@property (strong) UIWindow *window;
@end
@implementation AD
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	UIViewController *vc = [UIViewController new];
	vc.view.backgroundColor = [UIColor colorWithRed:0.1 green:0.6 blue:0.2 alpha:1];
	UILabel *l = [[UILabel alloc] initWithFrame:vc.view.bounds];
	l.text = @"Sideload OK"; l.textAlignment = NSTextAlignmentCenter; l.textColor = UIColor.whiteColor; l.font = [UIFont boldSystemFontOfSize:48];
	l.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	[vc.view addSubview:l];
	self.window.rootViewController = vc; [self.window makeKeyAndVisible];
	return YES;
}
@end
int main(int c, char **v) { @autoreleasepool { return UIApplicationMain(c, v, nil, @"AD"); } }
