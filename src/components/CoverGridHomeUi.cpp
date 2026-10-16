#include "CoverGridHomeUi.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "UITheme.h"
#include "icons/blocks.h"
#include "icons/AppsIcon.h"
#include "icons/book.h"
#include "icons/folder.h"
#include "icons/library.h"
#include "icons/settings2.h"
#include "icons/transfer.h"
#include "util/BookProgress.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId SELECT = 1;
// Grid cell padding around each cover; also feeds the screen's horizontal
// inset so the cover columns land on the header chrome's inset line.
constexpr int16_t COVER_CELL_INSET = 6;
constexpr int COVER_ROW_EXTRA_HEIGHT = 8;
}  // namespace

CoverGridHomeUi::CoverGridHomeUi(GfxRenderer& renderer)
    : UiAppHost(renderer), coverCache(renderer), renderer(renderer) {}

void CoverGridHomeUi::begin(const std::vector<RecentBook>& recent, bool opds, bool continuing) {
  books = &recent;
  hasOpds = opds;
  hasContinueReading = continuing;
  if (!recent.empty()) coverCache.begin();
  resetUi();
  app.on(SELECT, &CoverGridHomeUi::onAction, this);
  app.setScreen(&CoverGridHomeUi::screenFn, this);
  refreshCoverPaths();
  progress = hasContinueReading && !books->empty() ? loadBookProgress(books->front().path) : -1;
  if (progress >= 0) snprintf(progressText, sizeof(progressText), "%d%%", progress);
}

void CoverGridHomeUi::refreshCoverPaths() {
  coverCache.invalidate();
  for (size_t i = 0; i < books->size() && i < coverPaths.size(); ++i) refreshCoverPath(i);
}

void CoverGridHomeUi::refreshCoverPath(size_t index) {
  if (index >= books->size() || index >= coverPaths.size()) return;
  coverCache.invalidate(index);
  coverPaths[index] =
      thumbHeight > 0 ? UITheme::getCoverThumbPath((*books)[index].coverBmpPath, thumbHeight) : std::string();
}

int CoverGridHomeUi::thumbHeightFor() const { return thumbHeight > 0 ? thumbHeight : THUMB_HEIGHT; }

bool CoverGridHomeUi::takeThumbHeightChanged() { return std::exchange(thumbHeightChanged, false); }

void CoverGridHomeUi::noteThumbHeight(int slotHeight) {
  // One shared height for every slot (hero and grid covers are the same
  // size). The +8 surplus gives the paint clip a little bleed so the art's
  // rightward nudge never exposes the left edge; covers narrower than the
  // 2:3 slot may show a thin sliver at the sides rather than being cropped.
  const int height = std::max(1, slotHeight + 8);
  if (thumbHeight != height) {
    thumbHeight = height;
    thumbHeightChanged = true;
    refreshCoverPaths();
  }
}

void CoverGridHomeUi::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<CoverGridHomeUi*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

int CoverGridHomeUi::selectedAction(const MappedInputManager& input) {
  pending = -1;
  const auto touch = routeTouch(input);
  return touch.snap.touchReleased ? pending : -1;
}

void CoverGridHomeUi::screenFn(UiScreen& screen, void* user) { static_cast<CoverGridHomeUi*>(user)->draw(screen); }

void CoverGridHomeUi::draw(UiScreen& screen) {
  coverCache.prepare();
  const auto& theme = screen.theme();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y), static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  // Horizontal inset sized so the outer cover columns (content x plus the
  // grid's cell inset) sit on the theme's side-padding line: the heading and
  // tabs then align with the clock and battery, which tuck a few px further
  // in (headerStatusInset's optical bias).
  const int16_t hInset = std::max<int16_t>(
      0, static_cast<int16_t>(UITheme::getInstance().getMetrics().headerSidePadding - COVER_CELL_INSET - safe.x));
  // Drop the status band, hero, and grid down: touch boards get a full step
  // (no button-hint band to fit), button boards a modest nudge. The tabs stay
  // put (bottom-anchored below).
  const int16_t topInset =
      BoardConfig::hasTouch() ? static_cast<int16_t>(theme.spaceLg) : static_cast<int16_t>(theme.spaceSm + 4);
  screen.insetContent(fui::Insets{topInset, hInset, theme.spaceSm, hInset});
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  // Reserve the band's slot in the flow (its content draws at a fixed screen
  // position in drawHeaderBand); the slot doubles as padding above the heading.
  screen.takeTop(UITheme::getInstance().getMetrics().batteryBarHeight, theme.spaceSm);
  // Button boards run tighter above the tabs to make room for the top step.
  const int16_t tabGap = BoardConfig::hasTouch() ? theme.spaceSm : static_cast<int16_t>(4);
  auto tabRect = screen.takeBottom(UITheme::getInstance().getMetrics().coverGridTabBarHeight, tabGap);
  if (books->empty()) {
    drawTabs(screen, tabRect.inset(fui::Insets{0, COVER_CELL_INSET, 0, COVER_CELL_INSET}));
    drawEmpty(screen);
    drawHeaderBand();
    return;
  }
  // Hero cover matches the grid covers: the body splits into three equal
  // cover rows (hero + two grid rows), so the featured section is one row
  // tall and every cover on screen shares one size. The metadata floor still
  // applies when the rows would be too short for the hero's text lines.
  const fui::Rect body = screen.body();
  lastBody = body;  // sizing source for prepareHeroCard, which has no screen
  const int rowGap = std::max<int>(4, body.width / 100);
  grid.gap = grid.rowGap = rowGap;
  const int coverRowHeight = std::max(1, (body.height - theme.spaceSm - 2 * rowGap) / 3 + COVER_ROW_EXTRA_HEIGHT);
  const int16_t featuredHeight = std::min<int>(
      body.height, std::max<int>(coverRowHeight, screen.target().lineHeight(theme.bodyText.font) * (landscape ? 1 : 2) +
                                                     screen.target().lineHeight(theme.smallText.font) * 2 + 32));
  fui::Rect heroRect = screen.takeTop(featuredHeight, theme.spaceSm);
  prepareHeroCard(heroRect, coverRowHeight);
  // First page: the cover flow of books that were started and not finished.
  // Its walking chevron travels down to the home below, which is unchanged.
  if (isFlowPage()) {
    drawFlow(screen, heroRect);
    drawHeaderBand();
    return;
  }
  drawCurrent(screen, heroRect, coverRowHeight);
  drawGrid(screen);
  tabRect.x = gridBounds.x + grid.cellInset.left;
  tabRect.width = gridBounds.width - grid.cellInset.left - grid.cellInset.right;
  drawTabs(screen, tabRect);
  drawHeaderBand();
}

void CoverGridHomeUi::drawHeaderBand() {
  // The stock full-width band at the theme's topPadding, exactly like every
  // pushed screen's header: the battery/clock hold one position across the
  // whole UI, and the grid is widened to meet them (see the hInset above).
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.batteryBarHeight}, nullptr);
}

void CoverGridHomeUi::drawEmpty(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto body = screen.body();
  auto title = theme.titleText;
  title.bold = true;
  title.align = fui::TextAlign::Center;
  auto message = theme.bodyText;
  message.align = fui::TextAlign::Center;
  constexpr int16_t ICON_SIZE = 32;
  const int16_t titleHeight = screen.target().lineHeight(title.font);
  const int16_t messageHeight = screen.target().lineHeight(message.font);
  const int16_t contentHeight = ICON_SIZE + theme.spaceLg + titleHeight + theme.spaceSm + messageHeight;
  int16_t y = body.y + std::max(0, (body.height - contentHeight) / 2);
  renderer.drawIcon(BookIcon, body.x + (body.width - ICON_SIZE) / 2, y, ICON_SIZE);
  y += ICON_SIZE + theme.spaceLg;
  screen.target().text(fui::Rect{body.x, y, body.width, titleHeight}, tr(STR_NO_OPEN_BOOK), title);
  y += titleHeight + theme.spaceSm;
  screen.target().text(fui::Rect{body.x, y, body.width, messageHeight}, tr(STR_START_READING), message);
}

void CoverGridHomeUi::prepareHeroCard(fui::Rect& rect, const int coverRowHeight) {
  // Fit the shared 2:3 cover box within both the row and a grid column. No gap
  // reserve: the cell insets already keep adjacent covers apart, and the
  // SpaceBetween layout absorbs whatever slack remains.
  const int maxCoverWidth = rect.width / GRID_COLUMNS - 2 * COVER_CELL_INSET;
  card.coverSize.height = std::max(1, std::min(std::min<int>(rect.height, coverRowHeight) - 12, maxCoverWidth * 3 / 2));
  card.coverSize.width = std::max(1, card.coverSize.height * 2 / 3);
  noteThumbHeight(card.coverSize.height);
  gridBounds = layoutGrid(lastBody);
  rect.x = gridBounds.x;
  rect.width = gridBounds.width;
  card.coverPainterUserData = this;
  card.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::BookCardProps&, void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, 0);
  };
}

void CoverGridHomeUi::drawCurrent(UiScreen& screen, fui::Rect rect, const int coverRowHeight) {
  const auto& theme = screen.theme();
  const auto& book = books->front();
  card.title = book.title.c_str();
  card.author = book.author.empty() ? nullptr : book.author.c_str();
  card.meta = nullptr;
  card.progressLabel = progress >= 0 ? progressText : nullptr;
  card.centerTextOnCover = true;
  card.progress = std::max(0, progress);
  card.progressMax = progress >= 0 ? 100 : 0;
  card.action = SELECT;
  // The featured card's selected state is a slim accent bar drawn after the
  // card (see below), not a bookCard indicator: every ring/outline treatment
  // tried here either overwhelmed the large cover or made the heading above
  // read as misaligned.
  card.state = fui::StateNormal;
  card.styles = theme.listRow;
  card.styles.selected.background = fui::Paint::dither(fui::Color::LightGray);
  // The grid thumbs' selection ring draws with this border: gray like Lyra's
  // selection box, not solid black.
  card.styles.selected.border = fui::Paint::dither(fui::Color::LightGray);
  card.styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
  card.styles.selected.radius = theme.listRowRadius;
  card.styles.active = card.styles.selected;
  card.titleText = theme.bodyText;
  card.titleText.maxLines = renderer.getScreenWidth() > renderer.getScreenHeight() ? 1 : 2;
  card.authorText = theme.smallText;
  card.progressText = theme.smallText;
  card.progressHeight = 6;
  card.padding = fui::Insets{6, 6, 6, 6};
  card.gap = theme.spaceLg + theme.spaceSm;
  fui::bookCard(screen.frame(), rect, card);

  if (selected == 0 && !BoardConfig::hasTouch()) {
    // Button boards only: a vertical accent bar left of the card, cover-height
    // and vertically centered on it, marks the featured card as the button
    // cursor without framing the cover. Touch boards tap directly and need no
    // cursor on the hero card.
    const int16_t barH = card.coverSize.height;
    screen.target().fill(
        fui::Rect{static_cast<int16_t>(rect.x - 5), static_cast<int16_t>(rect.y + (rect.height - barH) / 2), 3, barH},
        fui::Paint::dither(fui::Color::LightGray));
  }
}

fui::Rect CoverGridHomeUi::layoutGrid(fui::Rect rect) {
  // Exactly the hero's box: the three-equal-rows split in draw() already
  // guarantees it fits, and sharing the size keeps one cached thumb per book.
  grid.cellInset = fui::Insets{COVER_CELL_INSET, COVER_CELL_INSET, COVER_CELL_INSET, COVER_CELL_INSET};
  grid.coverSize = card.coverSize;
  grid.rowHeight = grid.coverSize.height + 12;
  // Full content width: the SpaceBetween column layout pins the outer covers
  // to the rect edges, so the grid reaches the chrome's inset line instead of
  // centering at its natural width.
  rect.height = GRID_ROWS * grid.rowHeight + (GRID_ROWS - 1) * grid.rowGap;
  return rect;
}

void CoverGridHomeUi::drawGrid(UiScreen& screen) {
  const auto rect = gridBounds;
  grid.count = books->size() > 1 ? books->size() - 1 : 0;
  grid.columns = GRID_COLUMNS;
  grid.columnLayout = fui::CoverGridColumnLayout::SpaceBetween;
  grid.action = SELECT;
  grid.inputMask = fui::InputTouch;
  grid.selectedIndex = selected > 0 && selected < static_cast<int>(books->size()) ? selected - 1 : -1;
  // Same thick cover ring as the featured card; the dithered Cell background
  // was easy to miss behind a dark cover.
  grid.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  // Thick dithered ring sized for the small thumbs: 6px outside the cover,
  // 2px over its edge.
  grid.selectedCoverFrameGap = 6;
  grid.selectedCoverFrameWidth = 8;
  grid.cellStyles = card.styles;
  grid.labelHeight = 0;
  grid.labelGap = 0;
  grid.scrollIndicator = false;
  grid.itemProvider = [](uint16_t index, void*) { return fui::coverGridItem(nullptr, index + 1); };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::CoverGridItem&, uint16_t index,
                         void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, index + 1);
  };
  fui::coverGrid(screen.frame(), rect, grid);
}

void CoverGridHomeUi::drawTabs(UiScreen& screen, fui::Rect rect) {
  static constexpr const uint8_t* ICONS[] = {FolderIcon, LibraryIcon, BlocksIcon, TransferIcon, Settings2Icon, AppsIcon32};
  int count = 0;
  for (int i = 0; i < 6; ++i) {
    if (i == 2 && !hasOpds) continue;
    auto& tab = tabItems[count];
    tab.value = books->size() + count;
    tab.selected = selected == tab.value;
    tab.label = nullptr;
    ++count;
  }
  tabs.tabs = tabItems.data();
  tabs.count = count;
  tabs.layout = fui::TabBarLayout::SpaceBetween;
  tabs.action = SELECT;
  tabs.inputMask = fui::InputTouch;
  tabs.iconSize = 32;
  tabs.iconPainterUserData = this;
  tabs.iconPainter = [](fui::DrawTarget&, fui::Rect iconRect, const fui::TabItem& tab, uint8_t, void* user) {
    const auto& self = *static_cast<CoverGridHomeUi*>(user);
    const int index = tab.value - static_cast<int>(self.books->size());
    const int icon = !self.hasOpds && index >= 2 ? index + 1 : index;
    self.renderer.drawIcon(ICONS[icon], iconRect.x, iconRect.y, iconRect.width);
    return true;
  };
  tabs.tabStyles.normal.background = fui::Paint::solid(fui::Color::White);
  tabs.tabStyles.selected.background = fui::Paint::solid(fui::Color::White);
  tabs.selectedUnderline = 2;
  tabs.distributedSlotWidth = 0;
  fui::tabBar(screen.frame(), rect, tabs);
}

// --- first page: the cover flow ---------------------------------------------
//
// The books that were STARTED and not finished, as a carousel. It is the first
// thing the home shows; the wide chevron under the carousel travels down to the
// home that already existed, which is otherwise untouched.

void CoverGridHomeUi::buildFlow() {
  flowBooks.clear();
  flowPercent.clear();
  flowIndex = 0;
  if (books == nullptr) return;
  // "Started but not finished": loadBookProgress is the saved percentage, so
  // both edges are real data instead of a guess about which entries were opened.
  // Each entry costs an Epub metadata load, hence once per home entry and never
  // from the render task.
  for (const RecentBook& b : *books) {
    if (flowBooks.size() >= HomeCoverCache::MAX_COVERS) break;
    const int pct = loadBookProgress(b.path);
    if (pct <= 0 || pct >= 100) continue;
    flowBooks.push_back(b);
    flowPercent.push_back(pct);
  }
  LOG_INF("HOME", "cover flow: %u unfinished of %u recents", static_cast<unsigned>(flowBooks.size()),
          static_cast<unsigned>(books->size()));
  page = 0;  // when there is a flow, the flow is what the home opens on
}

void CoverGridHomeUi::flowMove(const int delta) {
  const int n = flowCount();
  if (n <= 0) return;
  flowIndex = std::max(0, std::min(n - 1, flowIndex + delta));
}

const RecentBook* CoverGridHomeUi::flowCurrent() const {
  if (flowIndex < 0 || flowIndex >= flowCount()) return nullptr;
  return &flowBooks[flowIndex];
}

void CoverGridHomeUi::drawFlow(UiScreen& screen, const fui::Rect heroRect) {
  if (flowBooks.empty()) return;
  const auto& theme = screen.theme();
  auto& target = screen.target();
  const int n = flowCount();
  if (flowIndex >= n) flowIndex = n - 1;
  if (flowIndex < 0) flowIndex = 0;

  // Title of the book the carousel is showing, centred above the covers.
  const int16_t titleH = target.lineHeight(theme.titleText.font);
  {
    auto title = theme.titleText;
    title.bold = true;
    title.align = fui::TextAlign::Center;
    title.maxLines = 2;
    target.text(fui::Rect{heroRect.x, heroRect.y, heroRect.width, static_cast<int16_t>(titleH * 2)},
                flowBooks[flowIndex].title.c_str(), title);
  }

  // Carousel: the neighbours are the SAME size as the centre cover. The thumb
  // cache holds one size and rescaling a dithered 1-bit cover aliases badly, so
  // the fan comes from pushing them off the edges, not from shrinking them.
  const int16_t cw = card.coverSize.width;
  const int16_t ch = card.coverSize.height;
  const int16_t cx = static_cast<int16_t>(heroRect.x + (heroRect.width - cw) / 2);
  const int16_t cy = static_cast<int16_t>(heroRect.y + titleH * 2 + theme.spaceSm);
  const int16_t gap = static_cast<int16_t>(theme.spaceSm + 2);
  if (flowIndex > 0) {
    paintFlowCover(target, 0, flowIndex - 1, fui::Rect{static_cast<int16_t>(cx - cw - gap), cy, cw, ch}, false);
  }
  if (flowIndex + 1 < n) {
    paintFlowCover(target, 1, flowIndex + 1, fui::Rect{static_cast<int16_t>(cx + cw + gap), cy, cw, ch}, false);
  }
  paintFlowCover(target, 2, flowIndex, fui::Rect{cx, cy, cw, ch}, true);

  // Reading line under the carousel...
  int16_t y = static_cast<int16_t>(cy + ch + theme.spaceSm);
  {
    auto meta = theme.smallText;
    meta.align = fui::TextAlign::Center;
    char line[24];
    snprintf(line, sizeof(line), "%d%%", flowPercent[flowIndex]);
    const int16_t h = target.lineHeight(meta.font);
    target.text(fui::Rect{heroRect.x, y, heroRect.width, h}, line, meta);
    y = static_cast<int16_t>(y + h);
  }
  // ...then the chevron: a WIDE two-stroke ∨, no shaft and no filled triangle,
  // drawn as two lines from a top-centre point down-left and down-right, wider
  // than the footer page-turn arrows, pointing at the home below.
  const int16_t chevY = static_cast<int16_t>(y + theme.spaceLg);
  const int16_t halfSpan = static_cast<int16_t>(std::min<int>(110, heroRect.width / 4));
  const int16_t drop = static_cast<int16_t>(halfSpan * 2 / 3);
  const int16_t midX = static_cast<int16_t>(heroRect.x + heroRect.width / 2);
  if (chevY + drop < screen.body().bottom()) {
    renderer.drawLine(midX - halfSpan, chevY, midX, static_cast<int16_t>(chevY + drop), true);
    renderer.drawLine(midX, static_cast<int16_t>(chevY + drop), midX + halfSpan, chevY, true);
  }
}

void CoverGridHomeUi::paintFlowCover(fui::DrawTarget& target, const size_t slot, const int index,
                                     const fui::Rect rect, const bool centre) {
  if (index < 0 || index >= flowCount()) return;
  const auto ink = fui::Paint::solid(fui::Color::Black);
  // Shadow: only the centre cover shows much of it, the neighbours are half
  // hidden behind it (and a wider shadow would just cost a wider repaint).
  const int16_t off = centre ? 3 : 2;
  target.fill(fui::Rect{rect.right(), static_cast<int16_t>(rect.y + off), off, rect.height}, ink);
  target.fill(fui::Rect{static_cast<int16_t>(rect.x + off), rect.bottom(), rect.width, off}, ink);
  const std::string path = UITheme::getCoverThumbPath(flowBooks[index].coverBmpPath, thumbHeightFor());
  if (path.empty()) return;
  coverCache.paint(rect, slot, path);
  if (centre) roundCoverCorners(rect, 10);
}

void CoverGridHomeUi::roundCoverCorners(const fui::Rect rect, const int radius) {
  if (radius <= 0 || rect.width < 2 * radius || rect.height < 2 * radius) return;
  const int16_t x0 = rect.x, y0 = rect.y;
  const int16_t x1 = static_cast<int16_t>(rect.right() - 1);
  const int16_t y1 = static_cast<int16_t>(rect.bottom() - 1);
  // Clear the four corner pockets that fall outside the rounded shape...
  for (int dy = 0; dy < radius; ++dy) {
    for (int dx = 0; dx < radius; ++dx) {
      const double ddx = radius - dx - 0.5;
      const double ddy = radius - dy - 0.5;
      if (ddx * ddx + ddy * ddy <= static_cast<double>(radius) * radius) continue;
      renderer.drawPixel(x0 + dx, y0 + dy, false);
      renderer.drawPixel(x1 - dx, y0 + dy, false);
      renderer.drawPixel(x0 + dx, y1 - dy, false);
      renderer.drawPixel(x1 - dx, y1 - dy, false);
    }
  }
  // ...then trace the arc that is left, so the centre cover reads as a card.
  const double rr = radius - 1.0;
  for (int a = 0; a <= 90; ++a) {
    const double rad = a * 3.14159265358979 / 180.0;
    const int offx = static_cast<int>(radius - 0.5 - rr * std::cos(rad) + 0.5);
    const int offy = static_cast<int>(radius - 0.5 - rr * std::sin(rad) + 0.5);
    renderer.drawPixel(x0 + offx, y0 + offy, true);
    renderer.drawPixel(x1 - offx, y0 + offy, true);
    renderer.drawPixel(x0 + offx, y1 - offy, true);
    renderer.drawPixel(x1 - offx, y1 - offy, true);
  }
}

bool CoverGridHomeUi::paintFramedCover(fui::DrawTarget& target, fui::Rect rect, size_t index) {
  constexpr int16_t SHADOW_OFFSET = 2;
  const auto ink = fui::Paint::solid(fui::Color::Black);
  target.fill(fui::Rect{rect.right(), static_cast<int16_t>(rect.y + SHADOW_OFFSET), SHADOW_OFFSET, rect.height}, ink);
  target.fill(fui::Rect{static_cast<int16_t>(rect.x + SHADOW_OFFSET), rect.bottom(), rect.width, SHADOW_OFFSET}, ink);
  // The false spine draws inside the cover paint (HomeCoverCache), glued to
  // the art's left edge, so it stays aligned whatever each cover's margin is.
  const bool drawn = index < coverPaths.size() && coverCache.paint(rect, index, coverPaths[index]);
  return drawn;
}
