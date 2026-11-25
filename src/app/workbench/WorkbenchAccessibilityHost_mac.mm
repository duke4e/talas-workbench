#include "app/workbench/WorkbenchAccessibilityHost.h"

#import <AppKit/AppKit.h>

namespace talas::app::workbench
{
bool workbenchHostReduceMotionPreference() noexcept
{
    @autoreleasepool
    {
        return NSWorkspace.sharedWorkspace
            .accessibilityDisplayShouldReduceMotion;
    }
}
}
