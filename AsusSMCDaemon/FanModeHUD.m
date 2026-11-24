#import "FanModeHUD.h"

static NSImage *fanIcon(void) {
    return [NSImage imageWithSize:NSMakeSize(64, 64) flipped:NO
                   drawingHandler:^BOOL(NSRect rect) {
        NSAffineTransform *center = [NSAffineTransform transform];
        [center translateXBy:NSMidX(rect) yBy:NSMidY(rect)];
        [center concat];
        [NSColor.whiteColor setFill];

        NSBezierPath *blade = [NSBezierPath bezierPath];
        [blade moveToPoint:NSMakePoint(2, 8)];
        [blade curveToPoint:NSMakePoint(12, 28)
              controlPoint1:NSMakePoint(1, 15) controlPoint2:NSMakePoint(3, 25)];
        [blade curveToPoint:NSMakePoint(24, 15)
              controlPoint1:NSMakePoint(19, 30) controlPoint2:NSMakePoint(26, 22)];
        [blade curveToPoint:NSMakePoint(5, 6)
              controlPoint1:NSMakePoint(23, 9) controlPoint2:NSMakePoint(13, 11)];
        [blade closePath];

        NSAffineTransform *rotate = [NSAffineTransform transform];
        [rotate rotateByDegrees:72];
        for (NSInteger i = 0; i < 5; ++i) {
            [blade fill];
            [blade transformUsingAffineTransform:rotate];
        }
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(-5, -5, 10, 10)] fill];
        return YES;
    }];
}

@interface FanModeHUD () <NSAnimationDelegate>
@end

@implementation FanModeHUD {
    NSTextField *modeLabel;
    NSViewAnimation *fade;
}

- (instancetype)init {
    NSRect bounds = NSMakeRect(0, 0, 160, 160);
    self = [super initWithContentRect:bounds
                            styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                              backing:NSBackingStoreBuffered defer:NO];
    if (!self)
        return nil;

    self.opaque = NO;
    self.hasShadow = NO;
    self.backgroundColor = NSColor.clearColor;
    self.ignoresMouseEvents = YES;
    self.hidesOnDeactivate = NO;
    self.level = NSStatusWindowLevel;
    self.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle;

    NSVisualEffectView *bezel = [[NSVisualEffectView alloc] initWithFrame:bounds];
    bezel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
    if (@available(macOS 10.14, *))
        bezel.material = NSVisualEffectMaterialHUDWindow;
    else
        bezel.material = NSVisualEffectMaterialDark;
    bezel.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    bezel.state = NSVisualEffectStateActive;
    bezel.maskImage = [NSImage imageWithSize:bounds.size flipped:NO
                              drawingHandler:^BOOL(NSRect rect) {
        [NSColor.blackColor setFill];
        [[NSBezierPath bezierPathWithRoundedRect:rect xRadius:18 yRadius:18] fill];
        return YES;
    }];
    self.contentView = bezel;

    NSImageView *icon = [[NSImageView alloc] initWithFrame:NSMakeRect(42, 54, 76, 76)];
    icon.image = fanIcon();
    icon.image.template = YES;
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    [bezel addSubview:icon];

    modeLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 22, 160, 23)];
    modeLabel.editable = NO;
    modeLabel.selectable = NO;
    modeLabel.bordered = NO;
    modeLabel.drawsBackground = NO;
    modeLabel.alignment = NSTextAlignmentCenter;
    modeLabel.font = [NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
    modeLabel.textColor = NSColor.labelColor;
    [bezel addSubview:modeLabel];

    return self;
}

- (void)showMode:(enum AsusSMCFanMode)mode {
    NSScreen *screen = NSScreen.screens.firstObject;
    if (!screen)
        return;
    switch (mode) {
        case FanModeAutomatic: modeLabel.stringValue = @"Automatic"; break;
        case FanModeFullSpeed: modeLabel.stringValue = @"Full Speed"; break;
        default: return;
    }

    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(fadeOut) object:nil];
    [fade stopAnimation];
    fade = nil;
    self.alphaValue = 1;
    [self setFrameOrigin:NSMakePoint(round(NSMidX(screen.frame) - NSWidth(self.frame) / 2),
                                     round(NSMinY(screen.frame) + NSHeight(screen.frame) / 6))];
    [self orderFrontRegardless];
    [self performSelector:@selector(fadeOut) withObject:nil afterDelay:1.0];
}

- (void)fadeOut {
    fade = [[NSViewAnimation alloc] initWithViewAnimations:@[@{
        NSViewAnimationTargetKey: self,
        NSViewAnimationEffectKey: NSViewAnimationFadeOutEffect
    }]];
    fade.delegate = self;
    fade.duration = 0.2;
    fade.animationBlockingMode = NSAnimationNonblocking;
    [fade startAnimation];
}

- (void)animationDidEnd:(NSAnimation *)animation {
    if (animation == fade) {
        [self orderOut:nil];
        fade = nil;
    }
}

@end
