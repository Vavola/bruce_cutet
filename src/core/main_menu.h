#ifndef __MAIN_MENU_H__
#define __MAIN_MENU_H__

#include "menu_items/ConfigMenu.h"
#include "menu_items/FileMenu.h"
#include <MenuItemInterface.h>

#include "menu_items/WifiMenu.h"
class MainMenu {
public:
    FileMenu fileMenu;
    WifiMenu wifiMenu;
    ConfigMenu configMenu;

    MainMenu();
    ~MainMenu();

    void begin(void);
    std::vector<MenuItemInterface *> getItems(void) { return _menuItems; }
    void hideAppsMenu();

private:
    int _currentIndex = 0;
    int _totalItems = 0;
    std::vector<MenuItemInterface *> _menuItems;
};
extern MainMenu mainMenu;

#endif
