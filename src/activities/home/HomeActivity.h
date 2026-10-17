#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/CoverGridHomeUi.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
  std::unique_ptr<CoverGridHomeUi> coverGridUi;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool hasPlugins = false;
  // The home "library" slot (index 2) shows Plugins when any plugin is
  // installed, otherwise OPDS. The index converters gate on its presence.
  bool hasLibrarySlot() const { return hasPlugins || hasOpdsServers; }
  bool hasContinueReading = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  // --- first page: the cover flow of books started and not finished ---------
  // It lives here, not in the cover-grid component: the grid only exists for
  // ONE of the five themes, and this page is the home's first page whatever
  // theme is selected. Down travels to the home below, which is untouched.
  std::vector<RecentBook> flowBooks;
  std::vector<int> flowPct;
  // Thumbs that failed to generate this session: retrying them on every home
  // visit is what made the loading popup reappear and left covers blank.
  std::vector<std::string> thumbFailures;
  int flowIndex = 0;
  bool flowActive = true;
  bool flowHasBooks() const { return !flowBooks.empty(); }
  void buildFlow();
  void flowMove(int delta);
  bool ensureCoverThumbFor(const RecentBook& book, int thumbHeight);
  const RecentBook* flowCurrent() const;
  // The carousel's geometry, shared by the drawing and the touch targets so a
  // tap lands on what is actually painted.
  struct FlowGeom {
    int coverTop = 0;
    int coverH = 0;
    int coverW = 0;
    int coverX = 0;
    int bandTop = 0;  // below this is "travel down to the home"
  };
  FlowGeom flowGeometry() const;
  int flowCoverHeight() const;
  void drawFlow();
  void drawFlowCover(const RecentBook& book, Rect rect, int thumbHeight, bool centre);
  // Perspective side panel: the inner edge (the one touching the centre cover)
  // is the tall one and the outer edge recedes, which is what makes the row read
  // as a 3D carousel instead of three flat pictures.
  void drawFlowSide(const RecentBook& book, bool left, const FlowGeom& geom);
  // Small flat bold chevron: down (∨) travels to the home, up (∧) travels back to
  // the carousel from the home.
  void drawChevron(int centreX, int y, bool up);
  void roundCoverCorners(Rect rect, int radius);
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, bool hasOpdsUrl) {
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::LIBRARY) return i;
    ++i;
    if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsUrl ? i : 0;
    if (hasOpdsUrl) ++i;
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    ++i;
    if (item == HomeMenuItem::APPS) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::LIBRARY;
    if (hasOpdsUrl && idx == i++) return HomeMenuItem::OPDS_BROWSER;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i++) return HomeMenuItem::SETTINGS_MENU;
    if (idx == i) return HomeMenuItem::APPS;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onLibraryOpen();
  void onSettingsOpen();
  void onAppsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onPluginsOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
  void fillCoverGridFromLibrary();
  void resolveGridCoverPaths();
  void loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
