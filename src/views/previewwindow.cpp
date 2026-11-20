// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "previewwindow.h"
#include "application.h"
#include "imagepage.h"
#include "imagesequence.h"
#include "pdfdocument.h"
#include "pdfpage.h"
#include "pdfview.h"
#include "rotation.h"
#include "turnsaver.h"
#include "shortcutsdialog.h"
#include "sidebar.h"
#include "welcomepage.h"

#include "cutefish/files/task.h"
#include "cutefish/files/url.h"

#include "base/event_loop.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/url.h"
#include "ui/cell_layout.h"
#include "ui/about_dialog.h"
#include "ui/animation.h"
#include "ui/application.h"
#include "ui/clipboard.h"
#include "ui/color_provider.h"
#include "ui/file_chooser.h"
#include "ui/file_drop_area.h"
#include "ui/header_backdrop.h"
#include "ui/header_button.h"
#include "ui/header_button_group.h"
#include "ui/image.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/menu.h"
#include "ui/painter.h"
#include "ui/shortcut_convention.h"
#include "ui/sidebar_divider.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/window.h"
#include "ui/window_controls.h"
#include "ui/window_header.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <utility>


using namespace cutefish;
using namespace std::chrono_literals;
using cutefish::i18n::format;
using cutefish::i18n::tr;
using cutefish::i18n::trc;
using ui::rounding::centeredStart;

static constexpr double kMinimumContentWidth = 480;
static constexpr double kMinimumContentHeight = 360;
// The terminal's default window shape, at nine tenths of its size.
static constexpr ui::SizeF kWelcomeSize {684, 405};
static constexpr double kZoomStep = 1.5;
// The header's spacing: the window buttons' inset from the window's edge,
// used at both ends and past the sidebar's edge; one gap between neighbouring
// groups, text included; a smaller one between the texts of one run.
static constexpr double kHeaderInset = 9;
static constexpr double kGroupGap = 12;
static constexpr double kTextGap = 8;
// What the content keeps beside a shown sidebar at the window's minimum size,
// as in Files: the sidebar grows only as the window does.
static constexpr double kMinimumContentWidthBesideSidebar = kMinimumContentWidth - Sidebar::kDefaultWidth;

// The header. In full screen it has a strip of its own, the window's colour
// with a hairline along its bottom, to stay readable over the image; else
// the content's header backdrop lies under it.
class PreviewWindow::HeaderBar : public ui::View
{
public:
    explicit HeaderBar(double height)
    {
        header = addChild<ui::WindowHeader>(height);
        header->setButtonMargin(4);
        header->setButtonsCentered(true);
        header->setContentUnderButtons(true);
    }

    ui::WindowHeader* header;
    // Where the content starts, past the sidebar.
    double contentLeft = 0;
    bool strip = false;

protected:
    void layout() override { header->setGeometry(localRect()); }

    void paint(ui::Painter& painter) override
    {
        if (!strip)
            return;
        const ui::RectF rect = localRect();
        painter.fillRect({contentLeft, 0, rect.width - contentLeft, rect.height}, ui::Theme::current().background);
        const double line = 1 / painter.scale();
        painter.drawRoundedRect({contentLeft, rect.height - line, rect.width - contentLeft, line}, {0},
                                ui::color(ui::ColorId::HeaderSeparator));
    }
};

// The header's own part: the sidebar toggle after the window buttons, the
// arrows with the place in the sequence and the file name from the
// sidebar's edge, the zoom buttons at the end. A PDF has no arrows: its file
// name, then the page in the secondary colour on the same line.
class PreviewWindow::HeaderRow : public ui::View
{
public:
    explicit HeaderRow(PreviewWindow& owner, ui::WindowHeader* header)
        : m_header(header)
    {
        sidebarGroup = addChild<ui::HeaderButtonGroup>();
        // The toolkit's sidebar glyph is drawn on 24 pixels with a one-pixel line.
        sidebar = addButton(sidebarGroup, "sidebar", [&owner] { owner.toggleSidebar(); }, 24);
        arrows = addChild<ui::HeaderButtonGroup>();
        previous = addButton(arrows, "chevron-left", [&owner] { owner.step(-1); });
        next = addButton(arrows, "chevron-right", [&owner] { owner.step(1); });
        position = addChild<ui::Label>();
        title = addChild<ui::Label>();
        title->setWeight(700);
        title->setElide(ui::Label::Elide::Middle);
        page = addChild<ui::Label>();
        page->setVisible(false);
        tools = addChild<ui::HeaderButtonGroup>();
        zoomOut = addButton(tools, "zoom-out", [&owner] { owner.zoomOut(); });
        zoomIn = addButton(tools, "zoom-in", [&owner] { owner.zoomIn(); });
        rotate = addButton(tools, "rotate-left", [&owner] { owner.rotate(-1); });
        search = addButton(tools, "search", [&owner] { owner.showFind(); });
    }

    // Each plate shows while one of its buttons does.
    void updateGroups()
    {
        for (ui::HeaderButtonGroup* group : {sidebarGroup, arrows, tools})
            group->setVisible(group->sizeHint().width > 0);
    }

    ui::HeaderButtonGroup* sidebarGroup;
    ui::HeaderButton* sidebar;
    ui::HeaderButtonGroup* arrows;
    ui::HeaderButton* previous;
    ui::HeaderButton* next;
    ui::Label* position;
    ui::Label* title;
    // "Page 5 of 120", shown while showsPage is set and it fits.
    ui::Label* page;
    bool showsPage = false;
    ui::HeaderButtonGroup* tools;
    ui::HeaderButton* zoomOut;
    ui::HeaderButton* zoomIn;
    ui::HeaderButton* rotate;
    ui::HeaderButton* search;
    double sidebarWidth = 0;
    // Off in full screen, where the window buttons hide.
    bool windowButtons = true;

protected:
    void layout() override
    {
        const double height = geometry().height;
        const double scale = window() ? window()->scale() : 1;
        // On whole device pixels, so equal gaps come out equal.
        const auto snap = [scale](double value) { return std::round(value * scale) / scale; };
        // A plate is drawn on whole device pixels, its width rounded down.
        const auto plateWidth = [scale](const ui::HeaderButtonGroup* plate) {
            return std::floor(plate->sizeHint().width * scale) / scale;
        };
        const double top = centeredStart(height, ui::HeaderButtonGroup::kSegment);
        // The capitals centred on the buttons, as Settings' title is.
        const auto textTop = [height, scale](const ui::Label* label) {
            return std::round((height / 2 - label->capCenter()) * scale) / scale;
        };
        double x = kHeaderInset;
        if (windowButtons) {
            const double spacing = ui::Theme::current().coloredWindowControls ? ui::window_controls::kColoredSpacing
                                                                              : ui::window_controls::kSpacing;
            x = m_header->leadingInset() - spacing + kGroupGap;
        }
        place(sidebarGroup, snap(x), top);
        x = sidebarGroup->geometry().x + plateWidth(sidebarGroup) + kGroupGap;
        // From the sidebar's edge as the Files header's buttons are.
        x = snap(std::max(x, sidebarWidth + kHeaderInset));
        if (arrows->isVisible()) {
            place(arrows, x, top);
            x = snap(x + plateWidth(arrows) + kGroupGap);
        }
        if (position->isVisible()) {
            const ui::SizeF hint = position->sizeHint();
            position->setColor(ui::ColorId::Text);
            const double width = std::ceil(hint.width);
            position->setGeometry({x, textTop(position), width, hint.height});
            x = snap(x + width + kTextGap);
        }
        // The window shows only whole device pixels of a fractional width.
        double right = (std::floor(geometry().width * scale) - std::round(kHeaderInset * scale)) / scale;
        // Rotate for an image, search for a PDF, in one place.
        if (tools->isVisible()) {
            right = snap(right - plateWidth(tools));
            place(tools, right, top);
            right -= kGroupGap;
        }
        const ui::SizeF hint = title->sizeHint();
        title->setColor(ui::ColorId::Text);
        if (showsPage) {
            // In line with the page's left edge at fit.
            if (sidebarWidth > 0)
                x = std::max(x, snap(sidebarWidth + PdfView::kMargin));
            // The page text stays whole while it fits; the name takes the rest.
            const double available = std::max(0.0, right - x);
            const ui::SizeF pageHint = page->sizeHint();
            const double pageWidth = std::ceil(pageHint.width);
            page->setVisible(pageWidth <= available);
            const double nameWidth = page->isVisible() ? std::max(0.0, available - kTextGap - pageWidth)
                                                       : available;
            const double y = textTop(title);
            title->setGeometry({x, y, std::min(nameWidth, std::ceil(hint.width)), hint.height});
            if (page->isVisible()) {
                const std::optional<ui::Label::LineGeometry> line = title->lineGeometry(0);
                const double nameEnd = x + std::ceil(line ? line->width : 0);
                page->setColor(ui::ColorId::DisabledText);
                page->setGeometry({nameEnd + kTextGap, y, pageWidth, pageHint.height});
            }
            return;
        }
        page->setVisible(false);
        title->setGeometry({x, textTop(title), std::max(0.0, right - x), hint.height});
    }

private:
    static ui::HeaderButton* addButton(ui::HeaderButtonGroup* group, const char* glyph,
                                       std::function<void()> action, double glyphSize = 16)
    {
        ui::HeaderButton* button = group->addButton(glyph, glyphSize);
        button->onClicked = std::move(action);
        return button;
    }

    static void place(ui::HeaderButtonGroup* group, double x, double y)
    {
        const ui::SizeF size = group->sizeHint();
        group->setGeometry({x, y, size.width, size.height});
    }

    ui::WindowHeader* m_header;
};

// The window's content: the sidebar down the left edge, the page beside it
// under the header, and in full screen the page under the header too, on
// black. Files dropped on it open as File > Open opens them.
class PreviewWindow::Root : public ui::FileDropArea
{
public:
    explicit Root(PreviewWindow& owner)
        : m_owner(owner)
    {
        setTracksHover(true);
        sidebar = addChild<Sidebar>(kHeaderHeight);
        image = addChild<ImagePage>();
        pdf = addChild<PdfPage>();
        welcome = addChild<WelcomePage>();
        // Over the content, under the header.
        imageBackdrop = addChild<ui::HeaderBackdrop>(image->viewer());
        pdfBackdrop = addChild<ui::HeaderBackdrop>(pdf->view());
        bar = addChild<HeaderBar>(kHeaderHeight);
        row = bar->header->setContent<HeaderRow>(owner, bar->header);
        divider = addChild<ui::SidebarDivider>(Sidebar::kMinimumWidth, Sidebar::kMaximumWidth, Sidebar::kDefaultWidth);
        divider->setName("sidebarDivider");
        divider->onResized = [this](double width) {
            m_owner.m_sidebarWidth = width;
            sidebar->setResizing(true);
            requestLayout();
        };
        divider->onFinished = [this](double width) {
            m_owner.m_sidebarWidth = width;
            sidebar->setResizing(false);
            m_owner.m_application.settings().setSidebarWidth(width);
            requestLayout();
        };
        filter = [](const std::string& url) { return base::url::isLocalFile(url); };
        onDropped = [this](const std::vector<std::string>& urls) {
            std::vector<std::string> paths;
            for (const std::string& url : urls)
                paths.push_back(base::url::toLocalFile(url).value_or(std::string()));
            m_owner.m_application.open(paths);
        };
    }

    // As wide as asked, within what leaves the content its width at the
    // window's minimum size.
    double sidebarWidth() const
    {
        if (!sidebar->isVisible())
            return 0;
        const double widest = std::max(Sidebar::kMinimumWidth,
                                       std::min(Sidebar::kMaximumWidth,
                                                geometry().width - kMinimumContentWidthBesideSidebar));
        return std::min(m_owner.m_sidebarWidth, widest);
    }

    Sidebar* sidebar;
    ImagePage* image;
    PdfPage* pdf;
    WelcomePage* welcome;
    ui::HeaderBackdrop* imageBackdrop;
    ui::HeaderBackdrop* pdfBackdrop;
    HeaderBar* bar;
    HeaderRow* row;
    ui::SidebarDivider* divider;

protected:
    void layout() override
    {
        const ui::RectF rect = localRect();
        sidebar->setVisible(m_owner.m_sidebarShown && !m_owner.isWelcome());
        const bool pdfShown = !m_owner.isWelcome() && m_owner.kind() == DocumentKind::Pdf;
        image->setVisible(!m_owner.isWelcome() && !pdfShown);
        pdf->setVisible(pdfShown);
        welcome->setVisible(m_owner.isWelcome());
        const double left = sidebarWidth();
        const bool fullScreen = m_owner.isFullScreen();
        // The content runs under the header and rests below it.
        const double inset = fullScreen ? 0 : kHeaderHeight;
        sidebar->setGeometry({0, 0, left, rect.height});
        divider->setVisible(left > 0);
        divider->setSidebarWidth(left);
        divider->setGeometry({left - ui::SidebarDivider::kWidth / 2, 0, ui::SidebarDivider::kWidth, rect.height});
        const double width = std::max(0.0, rect.width - left);
        const ui::RectF content {left, 0, width, rect.height};
        image->setTopInset(inset);
        image->setGeometry(content);
        pdf->setTopInset(inset);
        pdf->view()->setPageShadows(!fullScreen);
        pdf->setGeometry(content);
        welcome->setGeometry({left, kHeaderHeight, width, std::max(0.0, rect.height - kHeaderHeight)});
        imageBackdrop->setVisible(!fullScreen && image->isVisible());
        pdfBackdrop->setVisible(!fullScreen && pdfShown);
        imageBackdrop->setGeometry({left, 0, width, kHeaderHeight});
        pdfBackdrop->setGeometry({left, 0, width, kHeaderHeight});
        bar->strip = fullScreen;
        bar->requestPaint();
        bar->contentLeft = left;
        bar->setGeometry({0, 0, rect.width, kHeaderHeight});
        row->sidebarWidth = left;
        row->windowButtons = !fullScreen;
        row->requestLayout();
        if (window())
            window()->setBackgroundBlur(rect, ui::Theme::current().windowRadius);
    }

    void paint(ui::Painter& painter) override
    {
        const double left = sidebarWidth();
        const ui::RectF rect = localRect();
        painter.fillRect({left, 0, rect.width - left, rect.height},
                         m_owner.isFullScreen() ? ui::Color {0, 0, 0, 255} : ui::Theme::current().background);
    }

    void hoverMove(const ui::PointerEvent& event) override
    {
        // Surface changes resend motion at the same spot; that is not activity.
        const ui::PointF position = mapToWindow(event.position);
        if (position.x == m_lastHover.x && position.y == m_lastHover.y)
            return;
        m_lastHover = position;
        m_owner.poke();
    }

private:
    PreviewWindow& m_owner;
    ui::PointF m_lastHover {-1, -1};
};

PreviewWindow::PreviewWindow(Application& application)
    : m_application(application)
    , m_window(std::make_unique<ui::Window>())
    , m_jump(std::make_unique<PdfRequest>())
{
    m_sidebarWidth = std::clamp(m_application.settings().sidebarWidth(Sidebar::kDefaultWidth), Sidebar::kMinimumWidth,
                                Sidebar::kMaximumWidth);
    m_window->setTitle(tr("Preview"));
    m_window->resize({720, 540});
    m_window->setMinimumSize({kMinimumContentWidth, kMinimumContentHeight + kHeaderHeight});
    m_window->setResizeAroundCenter(true);
    auto root = std::make_unique<Root>(*this);
    m_root = root.get();
    m_window->setContentView(std::move(root));

    ImagePage* page = m_root->image;
    page->viewportFor = [this](const ui::ImageInfo& info) { return viewportFor(info); };
    page->onShown = [this] { shown(); };
    page->viewer()->onSwipe = [this](int delta) { step(delta); };
    page->viewer()->onContextMenuRequested = [this](ui::PointF position) { openContextMenu(position); };
    m_root->pdf->onStateChanged = [this] { pdfStateChanged(); };
    m_root->pdf->onDocumentReplaced = [this] { m_root->sidebar->setDocument(nullptr); };
    PdfView* pages = m_root->pdf->view();
    pages->onCurrentPageChanged = [this] { updateHeader(); };
    pages->onContextMenuRequested = [this](ui::PointF position, const std::string& uri) {
        openPdfContextMenu(position, uri);
    };
    pages->onOpenUri = [this](const std::string& uri) { m_application.openUri(uri, m_window.get()); };
    m_root->sidebar->onActivated = [this](int index) { goTo(index); };
    m_root->sidebar->onDestinationActivated = [this](const pdf::Destination& destination) {
        PdfDocument* document = m_root->pdf->document();
        if (!document)
            return;
        *m_jump = document->resolve(destination,
                                    [this](int page, ui::PointF point) { m_root->pdf->view()->goTo(page, point); });
    };
    WelcomePage* welcome = m_root->welcome;
    welcome->onOpen = [this] { openDialog(); };
    welcome->onRecentClicked = [this](const std::string& path) { m_application.open({path}); };
    welcome->onClearRecent = [this] { m_application.settings().clearRecentFiles(); };
    welcome->setRecentFiles(m_application.settings().recentFiles());
    m_subscriptions.push_back(m_application.settings().recentFilesChanged.connect(
            [this] { m_root->welcome->setRecentFiles(m_application.settings().recentFiles()); }));
    m_subscriptions.push_back(ui::Application::instance()->languageChanged.connect([this] { retranslate(); }));
    m_subscriptions.push_back(m_application.turnSaver().written.connect(
            [this](const std::string& path, int error) { turnWritten(path, error); }));

    m_window->onCloseRequested = [this] { close(); };
    m_window->onActiveChanged = [this] { updateBackground(); };
    m_window->onStatesChanged = [this] {
        const bool fullScreen = isFullScreen();
        ui::WindowHeader* header = m_root->bar->header;
        header->setMinimizeVisible(!fullScreen);
        header->setMaximizeVisible(!fullScreen);
        header->setCloseVisible(!fullScreen);
        poke();
        m_root->requestLayout();
        m_root->requestPaint();
    };

    m_idle.setSingleShot(true);
    m_idle.setInterval(2000ms);
    m_idle.onTimeout = [this] {
        m_pointerActive = false;
        updateChrome();
    };

    buildMainMenu();
    bindShortcuts();
    updateBackground();
    updateHeader();
}

PreviewWindow::~PreviewWindow() = default;

ImagePage* PreviewWindow::imagePage() const
{
    return m_root->image;
}

PdfPage* PreviewWindow::pdfPage() const
{
    return m_root->pdf;
}

Sidebar* PreviewWindow::sidebar() const
{
    return m_root->sidebar;
}

std::string PreviewWindow::positionText() const
{
    if (m_root->row->showsPage)
        return m_root->row->page->isVisible() ? m_root->row->page->text() : std::string();
    return m_root->row->position->isVisible() ? m_root->row->position->text() : std::string();
}

ui::RectF PreviewWindow::titleGeometry() const
{
    const ui::View* label = m_root->row->title;
    const ui::PointF origin = label->mapToWindow({});
    return {origin.x, origin.y, label->geometry().width, label->geometry().height};
}

ui::RectF PreviewWindow::pageTextGeometry() const
{
    const ui::View* label = m_root->row->page;
    const ui::PointF origin = label->mapToWindow({});
    return {origin.x, origin.y, label->geometry().width, label->geometry().height};
}

double PreviewWindow::titleTextEnd() const
{
    const ui::Label* label = m_root->row->title;
    const std::optional<ui::Label::LineGeometry> line = label->lineGeometry(0);
    return label->mapToWindow({}).x + (line ? line->width : 0);
}

bool PreviewWindow::titleElided() const
{
    return m_root->row->title->isElided();
}

double PreviewWindow::titleBaseline() const
{
    const ui::Label* label = m_root->row->title;
    return label->mapToWindow({}).y + label->baselineOffset();
}

double PreviewWindow::pageTextBaseline() const
{
    const ui::Label* label = m_root->row->page;
    return label->mapToWindow({}).y + label->baselineOffset();
}

ui::View* PreviewWindow::sidebarButton() const
{
    return m_root->row->sidebar;
}

ui::View* PreviewWindow::zoomInButton() const
{
    return m_root->row->zoomIn;
}

bool PreviewWindow::showsArrows() const
{
    return m_root->row->previous->isVisible();
}

ui::RectF PreviewWindow::previousArrowGeometry() const
{
    const ui::View* arrow = m_root->row->previous;
    const ui::PointF origin = arrow->mapToWindow({});
    return {origin.x, origin.y, arrow->geometry().width, arrow->geometry().height};
}

ui::RectF PreviewWindow::positionGeometry() const
{
    const ui::View* label = m_root->row->position;
    const ui::PointF origin = label->mapToWindow({});
    return {origin.x, origin.y, label->geometry().width, label->geometry().height};
}

std::string PreviewWindow::titleText() const
{
    return m_root->row->title->text();
}

std::string PreviewWindow::currentPath() const
{
    return m_sequence ? m_sequence->currentPath() : std::string();
}

int PreviewWindow::currentIndex() const
{
    if (!m_sequence)
        return -1;
    if (m_kind == DocumentKind::Pdf)
        return m_root->pdf->isReady() ? m_root->pdf->view()->currentPage() : 0;
    const std::vector<std::string>& paths = m_sequence->paths();
    const auto found = std::find(paths.begin(), paths.end(), m_root->image->shown().path);
    return found == paths.end() ? m_sequence->currentIndex() : int(found - paths.begin());
}

int PreviewWindow::count() const
{
    if (m_sequence && m_kind == DocumentKind::Pdf)
        return m_root->pdf->isReady() ? m_root->pdf->document()->pageCount() : 0;
    return m_sequence ? m_sequence->count() : 0;
}

void PreviewWindow::show(const std::vector<std::string>& paths)
{
    // Only the welcome page's size is kept.
    m_window->setRestoresSize({});
    std::error_code error;
    const bool folder = paths.size() == 1 && ImageSequence::isImage(paths.front())
            && std::filesystem::is_regular_file(paths.front(), error);
    m_sequenceConnections.clear();
    m_sequence = folder ? ImageSequence::folderOf(paths.front()) : ImageSequence::files(paths);
    m_sequenceConnections.push_back(m_sequence->changed.connect([this] { sequenceChanged(); }));
    m_sequenceConnections.push_back(m_sequence->currentWritten.connect([this] { currentWritten(); }));
    m_kind = paths.size() == 1 && PdfDocument::isPdf(paths.front()) ? DocumentKind::Pdf : DocumentKind::Image;
    m_opened = std::set<std::string>(paths.begin(), paths.end());
    m_mapPending = !m_mapped;
    if (m_kind == DocumentKind::Pdf) {
        // Until the document says how many pages it has.
        m_sidebarShown = false;
        m_root->image->clear();
        m_root->sidebar->setItems({});
        m_root->pdf->load(m_sequence->currentPath());
    } else {
        m_sidebarShown = m_application.settings().sidebarShown(kind());
        m_root->pdf->clear();
        m_root->sidebar->setItems(m_sequence->paths());
        m_root->image->load(m_sequence->currentPath());
    }
    m_root->requestLayout();
    updateHeader();
}

void PreviewWindow::raise(const std::string& activationToken)
{
    if (m_mapPending) {
        m_activationToken = activationToken;
        return;
    }
    if (!m_mapped && isWelcome()) {
        m_window->resize(kWelcomeSize);
        m_window->setRestoresSize("Welcome");
    }
    m_window->show();
    m_mapped = true;
    m_window->activate(activationToken);
}

void PreviewWindow::sequenceChanged()
{
    if (m_sequence->isEmpty()) {
        // Not from inside the sequence's own signal.
        base::EventLoop::current()->post([this, alive = std::weak_ptr<bool>(m_alive)] {
            if (alive.expired() || !m_sequence || !m_sequence->isEmpty())
                return;
            m_sequenceConnections.clear();
            m_sequence.reset();
            // The next file sizes the window again, as the first one did.
            m_sized = false;
            m_kind = DocumentKind::Image;
            m_root->image->clear();
            m_root->pdf->clear();
            m_root->sidebar->setItems({});
            m_root->requestLayout();
            updateHeader();
            updateTitle();
        });
        return;
    }
    if (m_kind == DocumentKind::Pdf)
        return;
    m_root->sidebar->setItems(m_sequence->paths());
    if (m_sequence->currentPath() != m_root->image->shown().path)
        m_root->image->load(m_sequence->currentPath());
    else
        preloadNeighbours();
    updateHeader();
}

void PreviewWindow::map()
{
    if (!m_mapPending)
        return;
    m_mapPending = false;
    m_window->show();
    m_mapped = true;
    m_window->activate(std::exchange(m_activationToken, std::string()));
}

void PreviewWindow::shown()
{
    if (!m_sized) {
        m_sized = true;
        if (!isFullScreen() && !m_window->states().maximized)
            m_window->resize(windowSizeFor(m_root->image->shown().info));
    }
    map();
    const std::string& path = m_root->image->shown().path;
    // A file still being turned shows the turns not written yet.
    if (const int unwritten = m_application.turnSaver().unwrittenTurns(path))
        m_root->image->viewer()->setQuarterTurns(unwritten);
    if (m_root->image->showsImage() && m_opened.erase(path))
        m_application.settings().addRecentFile(path);
    preloadNeighbours();
    updateHeader();
    updateTitle();
}

void PreviewWindow::pdfStateChanged()
{
    PdfPage* page = m_root->pdf;
    PdfDocument* document = page->document();
    if (!document || document->state() == PdfDocument::State::Opening)
        return;
    const bool ready = document->state() == PdfDocument::State::Ready;
    if (ready) {
        m_root->sidebar->setDocument(document);
        m_sidebarShown = m_application.settings().sidebarShown(DocumentKind::Pdf, document->pageCount() > 1);
        if (!m_sized) {
            m_sized = true;
            if (!isFullScreen() && !m_window->states().maximized)
                m_window->resize(windowSizeForPage(document->pageSizes().front()));
        }
        if (m_opened.erase(document->path()))
            m_application.settings().addRecentFile(document->path());
    }
    map();
    // The dialog goes over the window, mapped first.
    if (document->state() == PdfDocument::State::Locked && !page->asksPassword())
        page->askPassword();
    m_root->requestLayout();
    m_root->requestPaint();
    updateHeader();
    updateTitle();
}

void PreviewWindow::preloadNeighbours()
{
    if (m_kind == DocumentKind::Pdf)
        return;
    const int index = currentIndex();
    std::vector<std::string> ahead;
    for (const int neighbour : {index + 1, index - 1}) {
        if (neighbour >= 0 && neighbour < count())
            ahead.push_back(m_sequence->paths()[size_t(neighbour)]);
    }
    m_root->image->preload(ahead);
}

void PreviewWindow::updateHeader()
{
    HeaderRow* row = m_root->row;
    const bool document = !isWelcome();
    const bool multiple = count() > 1;
    const bool pdf = document && m_kind == DocumentKind::Pdf;
    const bool zoomable = pdf ? m_root->pdf->isReady() : document && m_root->image->showsImage();
    row->sidebar->setVisible(document);
    // A PDF goes through its pages in other ways; its arrows would step pages.
    row->previous->setVisible(multiple && !pdf);
    row->next->setVisible(multiple && !pdf);
    row->position->setVisible(multiple && !pdf);
    const int index = currentIndex();
    row->previous->setEnabled(index > 0);
    row->next->setEnabled(index + 1 < count());
    // TRANSLATORS: the shown image's place among the window's images, such as "3 of 24".
    row->position->setText(format(tr("{index} of {count}"),
                                  {{"index", std::to_string(index + 1)}, {"count", std::to_string(count())}}));
    row->showsPage = pdf && m_root->pdf->isReady();
    // TRANSLATORS: after a PDF's name on the header's line: the page most in sight and the number of pages.
    row->page->setText(format(tr("Page {page} of {count}"),
                              {{"page", std::to_string(index + 1)}, {"count", std::to_string(count())}}));
    row->title->setVisible(document);
    row->title->setText(shownName());
    row->zoomOut->setVisible(document);
    row->zoomIn->setVisible(document);
    row->zoomOut->setEnabled(zoomable);
    row->zoomIn->setEnabled(zoomable);
    row->rotate->setVisible(document && !pdf);
    row->rotate->setEnabled(canRotate());
    row->search->setVisible(pdf);
    row->search->setEnabled(pdf && m_root->pdf->isReady());
    if (m_root->sidebar->current() != index)
        m_root->sidebar->setCurrent(index);
    row->updateGroups();
    row->requestLayout();
}

std::string PreviewWindow::shownName() const
{
    if (m_kind == DocumentKind::Pdf)
        return std::filesystem::path(m_root->pdf->path()).filename().string();
    return m_root->image->shown().name;
}

void PreviewWindow::updateTitle()
{
    const std::string name = shownName();
    m_window->setTitle(isWelcome() || name.empty() ? std::string(tr("Preview")) : name);
}

void PreviewWindow::updateBackground()
{
    m_window->setBackgroundColor(m_window->isActive() ? ui::ColorId::TranslucentWindowBackground
                                                      : ui::ColorId::TranslucentWindowBackgroundInactive);
    // Views colour their text by it as they lay out.
    m_window->requestLayout();
}

ui::SizeF PreviewWindow::windowSizeFor(const ui::ImageInfo& info) const
{
    ui::SizeF area = m_window->availableSize();
    if (area.width <= 0 || area.height <= 0)
        area = {1280, 800};
    const double sidebar = m_sidebarShown ? m_sidebarWidth : 0;
    ui::SizeF content {kMinimumContentWidth, kMinimumContentHeight};
    if (info.readable && info.width > 0 && info.height > 0) {
        // One image pixel to a device pixel, shrunk into a large share of the work area.
        const double ratio = m_window->scale() > 0 ? m_window->scale() : 1;
        const double width = info.width / ratio;
        const double height = info.height / ratio;
        const double scale = std::min({1.0, (std::floor(area.width * 0.75) - sidebar) / width,
                                       (std::floor(area.height * 0.8) - kHeaderHeight) / height});
        content = {std::max(kMinimumContentWidth, std::round(width * scale)),
                   std::max(kMinimumContentHeight, std::round(height * scale))};
    } else if (info.readable) {
        // Without a size in its header the image fits this.
        content = {720, 540};
    }
    return {content.width + sidebar, content.height + kHeaderHeight};
}

// The first page at 100% beside the sidebar, at most 85% of the work area's
// height, and within its width.
ui::SizeF PreviewWindow::windowSizeForPage(ui::SizeF page) const
{
    ui::SizeF area = m_window->availableSize();
    if (area.width <= 0 || area.height <= 0)
        area = {1280, 800};
    const double sidebar = m_sidebarShown ? m_sidebarWidth : 0;
    const double width = std::min(page.width + 2 * PdfView::kMargin, std::floor(area.width * 0.9) - sidebar);
    const double height = std::min(page.height + 2 * PdfView::kMargin, std::floor(area.height * 0.85) - kHeaderHeight);
    return {std::max(kMinimumContentWidth, std::round(width)) + sidebar,
            std::max(kMinimumContentHeight, std::round(height)) + kHeaderHeight};
}

ui::SizeF PreviewWindow::viewportFor(const ui::ImageInfo& info) const
{
    if (m_sized || isFullScreen() || m_window->states().maximized)
        return m_root->image->viewport();
    const ui::SizeF size = windowSizeFor(info);
    return {size.width - (m_sidebarShown ? m_sidebarWidth : 0), size.height - kHeaderHeight};
}

void PreviewWindow::step(int delta)
{
    if (m_sequence)
        goTo(currentIndex() + delta);
}

void PreviewWindow::goTo(int index)
{
    if (!m_sequence)
        return;
    if (m_kind == DocumentKind::Pdf)
        m_root->pdf->view()->goToPage(index);
    else
        m_sequence->setCurrentIndex(index);
}

void PreviewWindow::pageStep(int delta)
{
    if (m_kind == DocumentKind::Pdf && m_sequence)
        m_root->pdf->view()->scrollScreen(delta);
    else
        step(delta);
}

void PreviewWindow::zoomIn()
{
    PdfView* pages = m_root->pdf->view();
    ImageViewer* viewer = m_root->image->viewer();
    if (m_kind == DocumentKind::Pdf && m_root->pdf->isReady())
        pages->zoomBy(kZoomStep, pages->center(), true);
    else if (m_kind == DocumentKind::Image && m_root->image->showsImage())
        viewer->zoomBy(kZoomStep, viewer->center(), true);
}

void PreviewWindow::zoomOut()
{
    PdfView* pages = m_root->pdf->view();
    ImageViewer* viewer = m_root->image->viewer();
    if (m_kind == DocumentKind::Pdf && m_root->pdf->isReady())
        pages->zoomBy(1 / kZoomStep, pages->center(), true);
    else if (m_kind == DocumentKind::Image && m_root->image->showsImage())
        viewer->zoomBy(1 / kZoomStep, viewer->center(), true);
}

void PreviewWindow::zoomToFit()
{
    if (m_kind == DocumentKind::Pdf && m_root->pdf->isReady())
        m_root->pdf->view()->zoomToFit();
    else if (m_kind == DocumentKind::Image && m_root->image->showsImage())
        m_root->image->viewer()->zoomToFit();
}

void PreviewWindow::zoomToActualSize()
{
    if (m_kind == DocumentKind::Pdf && m_root->pdf->isReady())
        m_root->pdf->view()->zoomToActualSize();
    else if (m_kind == DocumentKind::Image && m_root->image->showsImage())
        m_root->image->viewer()->zoomToActualSize();
}

void PreviewWindow::toggleSidebar()
{
    if (isWelcome())
        return;
    m_sidebarShown = !m_sidebarShown;
    m_application.settings().setSidebarShown(kind(), m_sidebarShown);
    m_root->requestLayout();
    m_root->requestPaint();
    updateHeader();
}

bool PreviewWindow::isFullScreen() const
{
    return m_window->states().fullScreen;
}

void PreviewWindow::toggleFullScreen()
{
    if (!isWelcome() || isFullScreen())
        m_window->setFullScreen(!isFullScreen());
}

// Image as PNG for any program to paste, as the file's own type besides, and
// the file; read and encoded on a worker.
void PreviewWindow::copy()
{
    if (m_kind == DocumentKind::Pdf) {
        m_root->pdf->view()->copy();
        return;
    }
    if (!m_root->image->showsImage())
        return;
    const ShownFile& file = m_root->image->shown();
    struct Copied
    {
        std::string png;
        std::string own;
    };
    m_copy = base::ThreadPool::global().run(
            [path = file.path, type = file.mimeType] {
                Copied copied;
                copied.own = base::file::read(path).value_or(std::string());
                if (type == "image/png")
                    copied.png = std::exchange(copied.own, std::string());
                else
                    copied.png = ui::Image::fromFile(path).encodePng();
                return copied;
            },
            [path = file.path, type = file.mimeType](Copied copied) {
                ui::MimeData data;
                if (!copied.png.empty())
                    data.setData("image/png", std::move(copied.png));
                if (!copied.own.empty())
                    data.setData(type, std::move(copied.own));
                data.setUrls({base::url::fromLocalFile(path)});
                ui::Clipboard::setMimeData(data);
            });
}

void PreviewWindow::selectAll()
{
    if (m_kind == DocumentKind::Pdf && m_root->pdf->isReady())
        m_root->pdf->view()->selectAll();
}

void PreviewWindow::showFind()
{
    if (m_kind == DocumentKind::Pdf)
        m_root->pdf->showFind();
}

void PreviewWindow::findNext(bool backwards)
{
    if (m_kind == DocumentKind::Pdf)
        m_root->pdf->findNext(backwards);
}

void PreviewWindow::showGoToPage()
{
    if (m_kind == DocumentKind::Pdf)
        m_root->pdf->showGoToPage();
}

void PreviewWindow::setShowsContents(bool contents)
{
    if (m_kind != DocumentKind::Pdf)
        return;
    if (!m_sidebarShown)
        toggleSidebar();
    m_root->sidebar->setShowsContents(contents);
}

bool PreviewWindow::canRotate() const
{
    const ShownFile& file = m_root->image->shown();
    return !isWelcome() && m_kind == DocumentKind::Image && m_root->image->showsImage() && !file.info.animated
            && rotation::canTurn(file.path, file.mimeType);
}

// The picture turns at once; the file follows on the process's save thread.
void PreviewWindow::rotate(int delta)
{
    if (!canRotate())
        return;
    const ShownFile& file = m_root->image->shown();
    ImageViewer* viewer = m_root->image->viewer();
    viewer->setQuarterTurns(viewer->quarterTurns() + delta);
    m_application.turnSaver().turn(file.path, file.mimeType, delta);
}

void PreviewWindow::turnWritten(const std::string& path, int error)
{
    if (!m_sequence)
        return;
    const std::vector<std::string>& paths = m_sequence->paths();
    if (std::find(paths.begin(), paths.end(), path) == paths.end())
        return;
    if (error) {
        if (!m_toast)
            m_toast = std::make_unique<ui::Toast>(m_window.get());
        const std::string name = std::filesystem::path(path).filename().string();
        if (error == rotation::kUnreadable) {
            m_toast->show(format(tr("Could not rotate {name}: it cannot be read as a JPEG or PNG image"),
                                 {{"name", name}}));
        } else {
            // TRANSLATORS: {reason} is the system's error text, such as "Permission denied".
            m_toast->show(format(tr("Could not rotate {name}: {reason}"),
                                 {{"name", name}, {"reason", std::strerror(error)}}));
        }
    }
    m_root->sidebar->refresh();
    // Decoded again as the file now is, which looks as it did turned; the
    // zoom and the place stay. Not while another file is on its way in.
    if (m_root->image->shown().path == path && currentPath() == path)
        m_root->image->reload(path);
}

void PreviewWindow::currentWritten()
{
    const std::string path = m_sequence->currentPath();
    if (m_kind == DocumentKind::Pdf) {
        m_root->pdf->load(path);
        return;
    }
    const TurnSaver& saver = m_application.turnSaver();
    // Our own writes: each brings its reload.
    if (saver.unwrittenTurns(path) != 0 || saver.writtenStamp(path) == TurnSaver::fileStamp(path))
        return;
    m_root->image->load(path);
}

void PreviewWindow::showInFiles()
{
    if (!isWelcome())
        m_application.showInFiles(currentPath());
}

void PreviewWindow::moveToTrash()
{
    const std::string path = m_kind == DocumentKind::Pdf ? currentPath() : m_root->image->shown().path;
    if (isWelcome() || path.empty())
        return;
    files::Task* task = files::trash({files::Url::fromLocalFile(path)});
    task->whenFinished([this, path, alive = std::weak_ptr<bool>(m_alive)](files::Task* finished) {
        if (!alive.expired() && m_sequence && finished->state() == files::Task::State::Succeeded)
            m_sequence->remove(path);
    });
}

void PreviewWindow::openDialog()
{
    m_chooser = std::make_unique<ui::FileChooser>(m_window.get());
    m_chooser->setMode(ui::FileChooser::Mode::OpenFiles);
    m_chooser->setTitle(tr("Open"));
    ui::FileChooser::Filter images;
    images.name = tr("Images");
    images.mimeTypes = ui::imageMimeTypes();
    ui::FileChooser::Filter documents;
    documents.name = tr("PDF Documents");
    documents.mimeTypes = {"application/pdf"};
    ui::FileChooser::Filter all;
    all.name = tr("Images and PDF Documents");
    all.mimeTypes = images.mimeTypes;
    all.mimeTypes.push_back("application/pdf");
    m_chooser->setFilters({all, images, documents});
    if (!isWelcome())
        m_chooser->setCurrentFolder(std::filesystem::path(currentPath()).parent_path().string());
    m_chooser->onAccepted = [this](const std::vector<std::string>& paths, const std::string&) {
        m_application.open(paths);
    };
    m_chooser->open();
}

void PreviewWindow::close()
{
    m_application.releaseWindow(this);
}

void PreviewWindow::openContextMenu(ui::PointF position)
{
    m_contextMenu = std::make_unique<ui::Menu>();
    m_contextMenu->addItem(tr("Copy"), [this] { copy(); });
    m_contextMenu->addSeparator();
    m_contextMenu->addItem(tr("Show in Files"), [this] { showInFiles(); });
    m_contextMenu->addItem(tr("Move to Trash"), [this] { moveToTrash(); });
    m_contextMenu->popup(m_window.get(), position);
}

void PreviewWindow::openPdfContextMenu(ui::PointF position, const std::string& uri)
{
    m_contextMenu = std::make_unique<ui::Menu>();
    if (!uri.empty()) {
        m_contextMenu->addItem(tr("Copy Link"), [uri] { ui::Clipboard::setText(uri); });
        m_contextMenu->addSeparator();
    }
    const size_t copyRow = m_contextMenu->addItem(tr("Copy"), [this] { copy(); });
    m_contextMenu->setItemEnabled(copyRow, m_root->pdf->view()->hasSelection());
    m_contextMenu->addItem(tr("Select All"), [this] { selectAll(); });
    m_contextMenu->popup(m_window.get(), position);
}

void PreviewWindow::showAbout()
{
    if (!m_about) {
        m_about = std::make_unique<ui::AboutDialog>(m_window.get());
        m_about->setName(tr("Preview"));
        m_about->setDescription(tr("View images and PDF documents"));
        m_about->setIconSource("cutefish-preview");
        m_about->onLinkActivated = [this](const std::string& link) {
            m_application.openUri(link, m_about->window() ? m_about->window() : m_window.get());
        };
    }
    m_about->show();
}

void PreviewWindow::showShortcuts()
{
    if (!m_shortcuts)
        m_shortcuts = std::make_unique<ShortcutsDialog>(m_window.get());
    m_shortcuts->show();
}

void PreviewWindow::poke()
{
    m_pointerActive = true;
    m_idle.start();
    updateChrome();
}

void PreviewWindow::updateChrome()
{
    const double target = !isFullScreen() || isWelcome() || m_pointerActive ? 1 : 0;
    if (target == m_chromeTo)
        return;
    HeaderBar* bar = m_root->bar;
    const auto apply = [this, bar](double opacity) {
        m_chromeOpacity = opacity;
        bar->setOpacity(opacity);
        bar->setVisible(opacity > 0);
    };
    m_chromeFrom = m_chromeOpacity;
    m_chromeTo = target;
    if (!m_chrome) {
        m_chrome = std::make_unique<ui::Animation>(m_window.get());
        m_chrome->setDuration(200ms);
        m_chrome->onUpdate = [this, apply](double value) { apply(m_chromeFrom + (m_chromeTo - m_chromeFrom) * value); };
    }
    m_chrome->stop();
    if (ui::Application::instance()->platform() == ui::Application::Platform::Headless)
        apply(target);
    else
        m_chrome->start();
}

void PreviewWindow::retranslate()
{
    if (m_contextMenu && m_contextMenu->isOpen())
        m_contextMenu->close();
    buildMainMenu();
    m_root->welcome->retranslate();
    m_root->image->retranslate();
    m_root->pdf->retranslate();
    updateHeader();
    updateTitle();
}

void PreviewWindow::buildMainMenu()
{
    // Starts reading the shortcut convention, which the trash key follows.
    ui::commandKeys();
    m_window->setMainMenu(nullptr);
    m_mainMenu = std::make_unique<ui::Menu>();
    const uint32_t ctrl = ui::ControlModifier;
    const uint32_t shift = ui::ShiftModifier;
    const uint32_t alt = ui::AltModifier;
    const auto image = [this] { return !isWelcome() && m_kind == DocumentKind::Image && m_root->image->showsImage(); };
    const auto pdf = [this] { return !isWelcome() && m_kind == DocumentKind::Pdf && m_root->pdf->isReady(); };

    ui::Menu* application = m_mainMenu->addSubmenu(tr("Preview"));
    application->setRole(ui::MenuRole::Application);
    application->addItem(tr("About Preview"), [this] { showAbout(); });
    application->addSeparator();
    application->addItem(tr("Hide Preview"), [this] { m_window->hideApplication(); });
    application->addSeparator();
    const size_t quit = application->addItem(tr("Quit Preview"), [this] { m_application.quit(); });
    application->setItemKeyEquivalent(quit, "q", ctrl);

    ui::Menu* file = m_mainMenu->addSubmenu(tr("File"));
    const size_t open = file->addItem(tr("Open…"), [this] { openDialog(); });
    file->setItemKeyEquivalent(open, "o", ctrl);
    const size_t recentRow = open + 1;
    ui::Menu* recent = file->addSubmenu(tr("Open Recent"));
    recent->onAboutToShow = [this, recent] {
        recent->clear();
        for (const std::string& path : m_application.settings().recentFiles()) {
            recent->addItem(std::filesystem::path(path).filename().string(),
                            [this, path] { m_application.open({path}); });
        }
        recent->addSeparator();
        recent->addItem(tr("Clear Recent Files"), [this] { m_application.settings().clearRecentFiles(); });
    };
    file->addSeparator();
    const size_t closeWindow = file->addItem(tr("Close Window"), [this] { close(); });
    file->setItemKeyEquivalent(closeWindow, "w", ctrl);
    file->addSeparator();
    const size_t show = file->addItem(tr("Show in Files"), [this] { showInFiles(); });
    const size_t trash = file->addItem(tr("Move to Trash"), [this] { moveToTrash(); });
    file->onAboutToShow = [this, file, recentRow, show, trash] {
        file->setItemEnabled(recentRow, !m_application.settings().recentFiles().empty());
        file->setItemEnabled(show, !isWelcome());
        file->setItemEnabled(trash, !isWelcome());
        // The trash key differs between the two shortcut conventions, as in Files.
        if (ui::commandKeys())
            file->setItemKeyEquivalent(trash, "BackSpace", ui::ControlModifier);
        else
            file->setItemKeyEquivalent(trash, "Delete", 0);
    };

    ui::Menu* edit = m_mainMenu->addSubmenu(tr("Edit"));
    const size_t copyRow = edit->addItem(tr("Copy"), [this] { copy(); });
    edit->setItemKeyEquivalent(copyRow, "c", ctrl);
    const size_t selectAll = edit->addItem(tr("Select All"), [this] { this->selectAll(); });
    edit->setItemKeyEquivalent(selectAll, "a", ctrl);
    edit->addSeparator();
    const size_t find = edit->addItem(tr("Find…"), [this] { showFind(); });
    edit->setItemKeyEquivalent(find, "f", ctrl);
    const size_t findNext = edit->addItem(tr("Find Next"), [this] { this->findNext(false); });
    edit->setItemKeyEquivalent(findNext, "g", ctrl);
    const size_t findPrevious = edit->addItem(tr("Find Previous"), [this] { this->findNext(true); });
    edit->setItemKeyEquivalent(findPrevious, "g", ctrl | shift);
    edit->onAboutToShow = [this, edit, image, pdf, copyRow, selectAll, find, findNext, findPrevious] {
        edit->setItemEnabled(copyRow, image() || (pdf() && m_root->pdf->view()->hasSelection()));
        // Text and search are a PDF's.
        for (const size_t row : {selectAll, find, findNext, findPrevious})
            edit->setItemEnabled(row, pdf());
    };

    ui::Menu* view = m_mainMenu->addSubmenu(tr("View"));
    const size_t sidebarRow = view->addItem(tr("Show Sidebar"), [this] { toggleSidebar(); });
    const size_t thumbnails = view->addItem(tr("Thumbnails"), [this] { setShowsContents(false); });
    const size_t contents = view->addItem(tr("Contents"), [this] { setShowsContents(true); });
    view->addSeparator();
    const size_t zoomInRow = view->addItem(tr("Zoom In"), [this] { zoomIn(); });
    view->setItemKeyEquivalent(zoomInRow, "=", ctrl);
    const size_t zoomOutRow = view->addItem(tr("Zoom Out"), [this] { zoomOut(); });
    view->setItemKeyEquivalent(zoomOutRow, "-", ctrl);
    const size_t fit = view->addItem(tr("Zoom to Fit"), [this] { zoomToFit(); });
    view->setItemKeyEquivalent(fit, "0", ctrl);
    const size_t actual = view->addItem(tr("Actual Size"), [this] { zoomToActualSize(); });
    view->setItemKeyEquivalent(actual, "9", ctrl);
    view->addSeparator();
    const size_t fullScreen = view->addItem(tr("Enter Full Screen"), [this] { toggleFullScreen(); });
    view->setItemKeyEquivalent(fullScreen, "f", 0);
    view->onAboutToShow = [this, view, image, pdf, sidebarRow, thumbnails, contents, zoomInRow, zoomOutRow, fit,
                           actual, fullScreen] {
        view->setItemText(sidebarRow, m_sidebarShown ? tr("Hide Sidebar") : tr("Show Sidebar"));
        view->setItemEnabled(sidebarRow, !isWelcome());
        // A PDF's sidebar shows thumbnails or its outline.
        view->setItemEnabled(thumbnails, pdf());
        view->setItemEnabled(contents, pdf() && m_root->sidebar->hasContents());
        view->setItemChecked(thumbnails, pdf() && m_sidebarShown && !m_root->sidebar->showsContents());
        view->setItemChecked(contents, pdf() && m_sidebarShown && m_root->sidebar->showsContents());
        for (const size_t row : {zoomInRow, zoomOutRow, fit, actual})
            view->setItemEnabled(row, image() || pdf());
        view->setItemEnabled(fullScreen, !isWelcome() || isFullScreen());
        view->setItemText(fullScreen, isFullScreen() ? tr("Exit Full Screen") : tr("Enter Full Screen"));
    };

    ui::Menu* go = m_mainMenu->addSubmenu(tr("Go"));
    const size_t previous = go->addItem(tr("Previous"), [this] { step(-1); });
    go->setItemKeyEquivalent(previous, "Left", 0);
    const size_t next = go->addItem(tr("Next"), [this] { step(1); });
    go->setItemKeyEquivalent(next, "Right", 0);
    const size_t first = go->addItem(tr("First"), [this] { goTo(0); });
    go->setItemKeyEquivalent(first, "Home", 0);
    const size_t last = go->addItem(tr("Last"), [this] { goTo(count() - 1); });
    go->setItemKeyEquivalent(last, "End", 0);
    go->addSeparator();
    const size_t goToPage = go->addItem(tr("Go to Page…"), [this] { showGoToPage(); });
    go->setItemKeyEquivalent(goToPage, "g", ctrl | alt);
    go->onAboutToShow = [this, go, pdf, previous, next, first, last, goToPage] {
        const int index = currentIndex();
        go->setItemEnabled(previous, index > 0);
        go->setItemEnabled(first, index > 0);
        go->setItemEnabled(next, index >= 0 && index + 1 < count());
        go->setItemEnabled(last, index >= 0 && index + 1 < count());
        // Pages are a PDF's.
        go->setItemEnabled(goToPage, pdf());
    };

    ui::Menu* tools = m_mainMenu->addSubmenu(tr("Tools"));
    const size_t rotateLeft = tools->addItem(tr("Rotate Left"), [this] { rotate(-1); });
    tools->setItemKeyEquivalent(rotateLeft, "l", ctrl);
    const size_t rotateRight = tools->addItem(tr("Rotate Right"), [this] { rotate(1); });
    tools->setItemKeyEquivalent(rotateRight, "r", ctrl);
    tools->onAboutToShow = [this, tools, rotateLeft, rotateRight] {
        tools->setItemEnabled(rotateLeft, canRotate());
        tools->setItemEnabled(rotateRight, canRotate());
    };

    ui::Menu* windowMenu = m_mainMenu->addSubmenu(tr("Window"));
    windowMenu->setRole(ui::MenuRole::Windows);
    windowMenu->addItem(tr("Minimize"), [this] { m_window->minimize(); });
    windowMenu->addItem(trc("window command", "Zoom"),
                        [this] { m_window->setMaximized(!m_window->states().maximized); });

    ui::Menu* help = m_mainMenu->addSubmenu(tr("Help"));
    help->setRole(ui::MenuRole::Help);
    const size_t shortcuts = help->addItem(tr("Keyboard Shortcuts"), [this] { showShortcuts(); });
    help->setItemKeyEquivalent(shortcuts, "?", ctrl);

    m_window->setMainMenu(m_mainMenu.get());
}

void PreviewWindow::bindShortcuts()
{
    ui::Window& window = *m_window;
    const uint32_t none = ui::NoModifier;
    const uint32_t ctrl = ui::ControlModifier;
    const uint32_t shift = ui::ShiftModifier;
    window.addShortcut(none, ui::key::Left, [this] { step(-1); });
    window.addShortcut(none, ui::key::Right, [this] { step(1); });
    window.addShortcut(none, ui::key::PageUp, [this] { pageStep(-1); });
    window.addShortcut(none, ui::key::PageDown, [this] { pageStep(1); });
    window.addShortcut(shift, ui::key::Space, [this] { pageStep(-1); });
    window.addShortcut(none, ui::key::Space, [this] { pageStep(1); });
    window.addShortcut(none, ui::key::Home, [this] { goTo(0); });
    window.addShortcut(none, ui::key::End, [this] { goTo(count() - 1); });
    window.addShortcut(ctrl, ui::key::letter('='), [this] { zoomIn(); });
    window.addShortcut(ctrl, ui::key::letter('+'), [this] { zoomIn(); });
    window.addShortcut(ctrl | shift, ui::key::letter('+'), [this] { zoomIn(); });
    window.addShortcut(ctrl, ui::key::letter('-'), [this] { zoomOut(); });
    window.addShortcut(ctrl, ui::key::letter('0'), [this] { zoomToFit(); });
    window.addShortcut(ctrl, ui::key::letter('9'), [this] { zoomToActualSize(); });
    window.addShortcut(ctrl, ui::key::letter('c'), [this] { copy(); });
    window.addShortcut(ctrl, ui::key::letter('a'), [this] { selectAll(); });
    window.addShortcut(ctrl, ui::key::letter('f'), [this] { showFind(); });
    window.addShortcut(ctrl, ui::key::letter('g'), [this] { findNext(false); });
    window.addShortcut(ctrl | shift, ui::key::letter('g'), [this] { findNext(true); });
    window.addShortcut(ctrl | ui::AltModifier, ui::key::letter('g'), [this] { showGoToPage(); });
    window.addShortcut(ctrl, ui::key::letter('l'), [this] { rotate(-1); });
    window.addShortcut(ctrl, ui::key::letter('r'), [this] { rotate(1); });
    window.addShortcut(none, ui::key::Delete, [this] { moveToTrash(); });
    window.addShortcut(ctrl, ui::key::BackSpace, [this] {
        if (ui::commandKeys())
            moveToTrash();
    });
    window.addShortcut(ctrl, ui::key::letter('o'), [this] { openDialog(); });
    window.addShortcut(ctrl, ui::key::letter('w'), [this] { close(); });
    window.addShortcut(ctrl, ui::key::letter('q'), [this] { m_application.quit(); });
    // Ctrl+? whichever way the keyboard makes the question mark.
    window.addShortcut(ctrl | shift, '?', [this] { showShortcuts(); });
    window.addShortcut(ctrl, '?', [this] { showShortcuts(); });
    window.addShortcut(ctrl | shift, '/', [this] { showShortcuts(); });
    // F and F11, as in the Video Player.
    window.addShortcut(none, ui::key::letter('f'), [this] { toggleFullScreen(); });
    window.addShortcut(none, ui::key::F1 + 10, [this] { toggleFullScreen(); });
    window.addShortcut(none, ui::key::Escape, [this] {
        if (isFullScreen())
            toggleFullScreen();
    });
}
