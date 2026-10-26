#import <UIKit/UIKit.h>
#include <stdint.h>
UIViewController *front_tv_root(void);
void front_tv_focus(int active);
static UILabel *stateLabel, *historyLabel;
static NSMutableArray *history;
void dm_pad_set(uint64_t b,int32_t lx,int32_t ly,int32_t rx,int32_t ry) {
    stateLabel.text=[NSString stringWithFormat:@"%llu,%d,%d,%d,%d",b,lx,ly,rx,ry];
    if (b||lx||ly||rx||ry) { if (!history)history=[NSMutableArray new];
        [history addObject:stateLabel.text?:@""];historyLabel.text=[history componentsJoinedByString:@";"]; }
}
void dm_input_set(uint64_t b,int32_t lx,int32_t ly,int32_t rx,int32_t ry) {}
void dm_focus_set(int a) {}
void guest_clock_freeze_for(int r,int a) {}
void dm_gfx_suspend(int a) {}
void front_tv_start(void) {}
int front_tv_ready(void) { return 1; }
int front_tv_preparing(void) { return 0; }
@interface QAScene : UIResponder <UIWindowSceneDelegate>
@property(strong,nonatomic) UIWindow *window;
@end
@implementation QAScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    self.window=[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController=front_tv_root();[self.window makeKeyAndVisible];
    stateLabel=[[UILabel alloc] initWithFrame:CGRectMake(100,100,1600,100)];stateLabel.textColor=UIColor.whiteColor;
    stateLabel.accessibilityIdentifier=@"qa-pad-state";stateLabel.text=@"0,0,0,0,0";
    historyLabel=[[UILabel alloc] initWithFrame:CGRectMake(100,220,1600,100)];historyLabel.textColor=UIColor.whiteColor;
    historyLabel.accessibilityIdentifier=@"qa-pad-history";historyLabel.text=@"none";
    [self.window.rootViewController.view addSubview:stateLabel];[self.window.rootViewController.view addSubview:historyLabel];
}
- (void)sceneWillResignActive:(UIScene *)scene { front_tv_focus(0); }
- (void)sceneDidBecomeActive:(UIScene *)scene { front_tv_focus(1); }
@end
@interface QAApp : UIResponder <UIApplicationDelegate>
@end
@implementation QAApp
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    UISceneConfiguration *c=[[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass=QAScene.class; return c;
}
@end
int main(int argc,char **argv) { @autoreleasepool { return UIApplicationMain(argc,argv,nil,@"QAApp"); } }
