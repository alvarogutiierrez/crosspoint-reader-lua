#pragma once

#include <array>
#include <string>
#include <vector>

#include "HomeCoverCache.h"
#include "RecentBooksStore.h"
#include "UiAppHost.h"
#include "components/bars/tab-bar.h"
#include "components/media/book-card.h"
#include "components/media/cover-grid.h"

class CoverGridHomeUi final : public UiAppHost {
 public:
  static constexpr int THUMB_HEIGHT = 400;
  static constexpr int GRID_COLUMNS = 3;
  static constexpr int GRID_ROWS = 2;
  static constexpr int MAX_BOOKS = 1 + GRID_COLUMNS * GRID_ROWS;
  static_assert(MAX_BOOKS <= HomeCoverCache::MAX_COVERS);
  explicit CoverGridHomeUi(GfxRenderer& renderer);
  void begin(const std::vector<RecentBook>& books, bool hasOpds, bool hasContinueReading);
  void refreshCoverPaths();

  // --- first page: cover flow of books STARTED but not finished -------------
  // The list is built once per home entry (each entry costs an Epub metadata
  // load, so it must never be done from the render task). Scrolling down reveals
  // the home that already exists; the wide chevron says so.
  void buildFlow();
  bool flowHasBooks() const { return !flowBooks.empty(); }
  int flowCount() const { return static_cast<int>(flowBooks.size()); }
  void flowMove(int delta);
  const RecentBook* flowCurrent() const;
  bool isFlowPage() const { return page == 0 && !flowBooks.empty(); }
  void setPage(int p) { page = p; }
  int getPage() const { return page; }
  void setSelection(int selection) { selected = selection; }
  int selectedAction(const MappedInputManager& input);
  // Exact generation height, shared by every slot and recorded during draw.
  // Thumbs must be generated at the drawn size: rescaling a dithered 1-bit
  // image aliases badly.
  int thumbHeightFor() const;
  bool takeThumbHeightChanged();

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawHeaderBand();
  void drawEmpty(UiScreen& screen);
  // Sizing half of the featured card (cover box, thumb height, grid bounds),
  // shared by the hero card and the cover-flow page so both use ONE cached
  // thumb size: a re-generated thumb costs an SD decode + dither.
  void prepareHeroCard(freeink::ui::Rect& rect, int coverRowHeight);
  void drawCurrent(UiScreen& screen, freeink::ui::Rect rect, int coverRowHeight);
  void drawFlow(UiScreen& screen, freeink::ui::Rect heroRect);
  void paintFlowCover(freeink::ui::DrawTarget& target, size_t slot, int index, freeink::ui::Rect rect,
                      bool centre);
  void roundCoverCorners(freeink::ui::Rect rect, int radius);
  void drawGrid(UiScreen& screen);
  freeink::ui::Rect layoutGrid(freeink::ui::Rect rect);
  void drawTabs(UiScreen& screen, freeink::ui::Rect rect);
  bool paintFramedCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect, size_t index);
  void refreshCoverPath(size_t index);
  void noteThumbHeight(int slotHeight);

  HomeCoverCache coverCache;
  GfxRenderer& renderer;
  const std::vector<RecentBook>* books = nullptr;
  std::array<std::string, MAX_BOOKS> coverPaths;
  int thumbHeight = 0;
  bool thumbHeightChanged = false;
  int selected = 0;
  int pending = -1;
  int progress = -1;
  // Cover-flow page state: the unfinished books, the carousel cursor and which
  // of the two pages is showing (0 = flow, 1 = the existing home).
  std::vector<RecentBook> flowBooks;
  std::vector<int> flowPercent;
  int flowIndex = 0;
  int page = 0;
  bool hasOpds = false;
  bool hasContinueReading = false;
  char progressText[12]{};
  // Component styles and interaction tables stay off the render task's stack.
  freeink::ui::BookCardProps card;
  freeink::ui::CoverGridProps grid;
  freeink::ui::Rect gridBounds{};
  freeink::ui::Rect lastBody{};  // body rect of the last draw, for prepareHeroCard
  freeink::ui::TabBarProps tabs;
  std::array<freeink::ui::TabItem, 6> tabItems;
};
