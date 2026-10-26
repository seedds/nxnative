#import <XCTest/XCTest.h>
@interface RemoteUITests:XCTestCase
@end
@implementation RemoteUITests
- (void)testProductionRemoteUI {
    self.continueAfterFailure=NO;
    XCUIApplication *app=[[XCUIApplication alloc] initWithBundleIdentifier:@"org.nxnative.tvremote-harness"];
    [app launch];
    XCTAssertTrue([app.staticTexts[@"remote-hint"] waitForExistenceWithTimeout:10]);
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonPlayPause];
    XCTAssertTrue([app.otherElements[@"remote-controls"] waitForExistenceWithTimeout:5]);
    XCTAssertTrue(app.buttons[@"remote-Resume / Move"].hasFocus);
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonRight];
    XCTAssertTrue(app.buttons[@"remote-Camera stick"].hasFocus);
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonSelect];
    XCTAssertFalse(app.otherElements[@"remote-controls"].exists);
    XCTAssertTrue([app.staticTexts[@"remote-hint"].label containsString:@"CAMERA"]);
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonRight];
    XCTAssertTrue([app.staticTexts[@"qa-pad-history"].label containsString:@"0,0,0,32767,0"]);
    XCTAssertEqualObjects(app.staticTexts[@"qa-pad-state"].label,@"0,0,0,0,0");
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonSelect];
    XCTAssertTrue([app.staticTexts[@"qa-pad-history"].label containsString:@"1,0,0,0,0"]);
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonMenu];
    XCTAssertTrue([app.staticTexts[@"qa-pad-history"].label containsString:@"2,0,0,0,0"]);
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonPlayPause];
    XCTAssertTrue([app.otherElements[@"remote-controls"] waitForExistenceWithTimeout:5]);
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonSelect];
    XCTAssertTrue([app.staticTexts[@"remote-hint"].label containsString:@"MOVE"]);
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonPlayPause];
    for(int i=0;i<4;i++) [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonDown];
    XCTAssertTrue(app.buttons[@"remote-Hold L"].hasFocus);
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonSelect];
    XCTAssertTrue([app.staticTexts[@"remote-hint"].label containsString:@"Holding: L"]);
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonMenu];
    XCTAssertFalse(app.otherElements[@"remote-controls"].exists);
    XCTAssertEqualObjects(app.staticTexts[@"qa-pad-state"].label,@"64,0,0,0,0");
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonPlayPause];
    for(int i=0;i<3;i++) [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonRight];
    XCTAssertTrue(app.buttons[@"remote-Release held buttons"].hasFocus);
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonSelect];
    XCTAssertEqualObjects(app.staticTexts[@"qa-pad-state"].label,@"0,0,0,0,0");
    XCTAttachment *a=[XCTAttachment attachmentWithScreenshot:XCUIScreen.mainScreen.screenshot];a.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:a];
    [XCUIRemote.sharedRemote pressButton:XCUIRemoteButtonMenu];
    XCTAssertFalse(app.otherElements[@"remote-controls"].exists);
}
@end
