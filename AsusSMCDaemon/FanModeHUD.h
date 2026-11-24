#import <AppKit/AppKit.h>
#import "../Global/AsusSMCMessage.h"

@interface FanModeHUD : NSPanel
- (void)showMode:(enum AsusSMCFanMode)mode;
@end
