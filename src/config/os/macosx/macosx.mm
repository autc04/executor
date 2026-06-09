#define _block_h_
#include <Cocoa/Cocoa.h>
#include <algorithm>
#include <functional>

static bool inited = false;
static bool dock = false;

void macosx_hide_menu_bar(int mouseX, int mouseY, int width, int height)
{
    // Apple says that if you hide the menu bar, you *have* to hide the dock
    // as well. Otherwise, you get an assertion failure.
    // I don't understand why they insist on that.
    // Executor would have no problem with the Dock, but we have our own
    // menu bar, so the menu bar has to be hidden.
    // As a hack, we'll switch between Autohidden dock and menu bar and
    // truly hidden dock and menu bar depending on the mouse Y coordinate.
    // If the mouse is near the menu bar, we set things to "hide" so the
    // menu bar doesn't pop up.
    // If the mouse is elsewhere, we set things to "autohide" so the user
    // can access the dock.

    @autoreleasepool
    {
        if(!inited)
        {
            [NSApp setPresentationOptions: NSApplicationPresentationHideMenuBar
                | NSApplicationPresentationHideDock];

            // Zap keyboard equivalents in the application menu.
            // Emulated mac apps are used to owning Command-H,
            // and they really need to handle Command-Q.
            NSMenuItem *menutitle = [[NSApp mainMenu] itemAtIndex: 0];
            NSMenu *menu = [menutitle submenu];
            for(NSMenuItem *item in [menu itemArray])
            {
                item.keyEquivalent = @"";
            }
        }
        inited = true;

        NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
        auto dockDefaults = [defaults persistentDomainForName:@"com.apple.dock"];
        NSString *orientation = [dockDefaults valueForKey:@"orientation"];
        
        int distanceFromDock;
        int tileSize = 64;
        NSNumber *tileSizeNum = [dockDefaults valueForKey:@"tilesize"];
        if(tileSizeNum)
            tileSize = [tileSizeNum intValue];

        if([@"left" isEqualTo: orientation])
            distanceFromDock = mouseX - 2 * tileSize;
        else if([@"right" isEqualTo: orientation])
            distanceFromDock = width - 2 * tileSize - mouseX;
        else
            distanceFromDock = height - 2 * tileSize - mouseY;
        
        int distanceFromMenu = mouseY - 22;

        //std::cout << "dock [" << (dock ? 'X':' ') << "]: " << distanceFromDock
        //          << " menu [" << (dock ? ' ':'X') << "]: " << distanceFromMenu << std::endl;
        if(dock)
        {
            if(distanceFromMenu < 20 || distanceFromDock > 40)
            {
                [NSApp setPresentationOptions: NSApplicationPresentationHideMenuBar
                    | NSApplicationPresentationHideDock];
                dock = false;
            }
        }
        else
        {
            if(distanceFromMenu > 40 && distanceFromDock < 20)
            {
                [NSApp setPresentationOptions: NSApplicationPresentationAutoHideMenuBar
                    | NSApplicationPresentationAutoHideDock];
                dock = true;
            }
        }
    }
}

void macosx_autorelease_pool(const std::function<void ()>& fun)
{
    @autoreleasepool
    {
        fun();
    }
}
