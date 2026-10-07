// whyhot-desktop: a small, soft, collapsible always-on-top companion window
// (Qt6 Widgets). Runs the same collectors and CorrelationEngine as
// `whyhot watch`, in-process, and records incidents to the same log.
//
// Always-on-top is ignored by GNOME's Wayland compositor for normal
// windows, so the app forces Qt's xcb (XWayland) backend, where it works.
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetrics>
#include <QIcon>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <memory>
#include <string>
#include <vector>

#include "collectors/cpu.h"
#include "collectors/fan.h"
#include "collectors/platform.h"
#include "collectors/process.h"
#include "collectors/thermal.h"
#include "correlation/engine.h"
#include "correlation/store.h"

using namespace whyhot;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void handleSignal(int) { g_stop = 1; }

constexpr int kMargin = 10;  // transparent margin around the card, holds the shadow
constexpr int kCollapsedW = 260, kCollapsedH = 84;
constexpr int kExpandedW = 332, kExpandedH = 486;

const QColor kInk(0x6b, 0x57, 0x66), kInkSoft(0xa5, 0x8f, 0xa0), kLine(0xf6, 0xd9, 0xe6),
    kAccent(0xff, 0x9f, 0xbf), kPaper2(0xfd, 0xee, 0xf5), kBlush(0xff, 0xb8, 0xcc);

// ---- view model ---------------------------------------------------------
enum class Mood { kSleepy, kChill, kWarm, kHot, kToasty };

struct View {
  bool have = false;
  double temp = -1, cpu = 0;
  int fan = -1;
  bool throttled = false;
  bool incident = false;
  std::string trigger, trigger_proc;
  int elapsed = 0;
  std::vector<std::pair<std::string, double>> procs;
  bool has_last = false;
  std::string last1, last2, last3;
  std::string profile, time;
};

Mood moodFor(const View& v) {
  if (!v.have || v.temp < 0) return Mood::kSleepy;
  if (v.throttled || v.temp >= 85) return Mood::kToasty;
  if (v.temp >= 75) return Mood::kHot;
  if (v.temp >= 62) return Mood::kWarm;
  return Mood::kChill;
}

QColor moodColor(Mood m) {
  switch (m) {
    case Mood::kChill: return QColor(0xc9, 0xf0, 0xdf);
    case Mood::kWarm: return QColor(0xff, 0xe6, 0xb8);
    case Mood::kHot: return QColor(0xff, 0xc4, 0xb8);
    case Mood::kToasty: return QColor(0xff, 0x9f, 0x9f);
    default: return QColor(0xd9, 0xd4, 0xee);
  }
}

QString sayFor(const View& v, Mood m) {
  if (!v.have || v.temp < 0) return QStringLiteral("zzz... waiting for my first reading ☁");
  if (v.incident) {
    QString lead = QStringLiteral("something is happening!");
    if (v.trigger == "temp_rise") lead = QStringLiteral("ooh, I got suddenly warm!");
    else if (v.trigger == "fan_rise") lead = QStringLiteral("the little fan just sped up!");
    else if (v.trigger == "process_cpu")
      lead = (v.trigger_proc.empty() ? QStringLiteral("someone") : QString::fromStdString(v.trigger_proc)) +
             QStringLiteral(" is working super hard!");
    return lead + QStringLiteral(" I'm taking notes (%1s)").arg(v.elapsed);
  }
  if (m == Mood::kToasty) return QStringLiteral("so hot... I need a nap in the fridge");
  if (m == Mood::kHot) {
    if (!v.procs.empty() && v.procs[0].second > 20)
      return QString::fromStdString(v.procs[0].first) + QStringLiteral(" is keeping me toasty!");
    return QStringLiteral("a bit sweaty, but I'm okay~");
  }
  if (v.fan > 3000 && v.cpu < 25) return QStringLiteral("fan is loud but nothing's busy... hmm?");
  if (m == Mood::kWarm) return QStringLiteral("cozy and warm ☁");
  return QStringLiteral("all calm and cool, yay ☁");
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

View buildView(const CorrelationEngine& engine, double fake_temp) {
  View v;
  if (const Sample* s = engine.latest()) {
    v.have = true;
    v.temp = fake_temp >= 0 ? fake_temp : s->thermal.cpu_temp_c;
    v.cpu = s->cpu.total_util_pct;
    v.fan = s->fan.available ? s->fan.rpm : -1;
    v.throttled = s->thermal.throttled;
    v.time = formatClockTime(s->ts);
    if (s->platform.available) v.profile = s->platform.profile;
    for (size_t i = 0; i < s->top_processes.size() && i < 3; ++i)
      v.procs.emplace_back(s->top_processes[i].comm, s->top_processes[i].cpu_pct);
    if (const Incident* a = engine.active()) {
      v.incident = true;
      v.trigger = a->trigger;
      v.trigger_proc = a->trigger_process;
      v.elapsed = static_cast<int>(
          std::chrono::duration_cast<std::chrono::seconds>(s->ts - a->start_ts).count());
    }
  }
  if (!engine.recentClosed().empty()) {
    const Incident& i = engine.recentClosed().front();
    v.has_last = true;
    v.last1 = "#" + std::to_string(i.id) + "  " + formatClockTime(i.start_ts) + " - " + formatClockTime(i.end_ts);
    v.last2 = "blame: " + i.primary_contributor;
    v.last3 = "peak " + std::to_string(static_cast<int>(i.peak_cpu_temp)) + "°C / " +
              std::to_string(static_cast<int>(i.peak_fan_rpm)) + " rpm, fan " + lower(toString(i.fan_response));
  }
  return v;
}

QString fanShort(int rpm) {
  if (rpm < 0) return QStringLiteral("--");
  if (rpm >= 1000) return QString::number(rpm / 1000.0, 'f', 1) + "k";
  return QString::number(rpm);
}

void paintMascot(QPainter& p, double cx, double cy, double size, Mood mood, const QColor& body, double t,
               bool incident) {
  p.save();
  const double bob = std::sin(t * 2.0) * 2.0;
  p.translate(cx, cy + bob);
  if (incident) p.rotate(std::sin(t * 12.0) * 6.0);
  p.scale(size / 100.0, size / 100.0);
  p.translate(-50, -50);

  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0, 0, 0, 20));
  p.drawEllipse(QPointF(50, 94 - bob), 26, 4);

  p.setBrush(body);  // ears
  QPainterPath ears;
  ears.moveTo(30, 14); ears.quadTo(26, 2, 38, 8); ears.closeSubpath();
  ears.moveTo(70, 14); ears.quadTo(74, 2, 62, 8); ears.closeSubpath();
  p.drawPath(ears);

  QPainterPath b;  // body
  b.moveTo(50, 10);
  b.cubicTo(74, 10, 90, 28, 90, 54);
  b.cubicTo(90, 78, 74, 90, 50, 90);
  b.cubicTo(26, 90, 10, 78, 10, 54);
  b.cubicTo(10, 28, 26, 10, 50, 10);
  p.setPen(QPen(Qt::white, 3));
  p.drawPath(b);

  p.setPen(Qt::NoPen);
  QColor blush = kBlush; blush.setAlphaF(0.75);
  p.setBrush(blush);
  p.drawEllipse(QPointF(26, 62), 8, 5);
  p.drawEllipse(QPointF(74, 62), 8, 5);

  QPen stroke(kInk, 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(stroke);
  p.setBrush(Qt::NoBrush);
  QPainterPath f;
  switch (mood) {
    case Mood::kChill:
      f.moveTo(30, 46); f.quadTo(36, 38, 42, 46);
      f.moveTo(58, 46); f.quadTo(64, 38, 70, 46);
      f.moveTo(44, 58); f.quadTo(50, 66, 56, 58);
      p.drawPath(f);
      break;
    case Mood::kWarm:
      p.setPen(Qt::NoPen);
      p.setBrush(kInk);
      p.drawEllipse(QPointF(36, 46), 4.5, 4.5);
      p.drawEllipse(QPointF(64, 46), 4.5, 4.5);
      p.setBrush(Qt::white);
      p.drawEllipse(QPointF(37.5, 44.5), 1.5, 1.5);
      p.drawEllipse(QPointF(65.5, 44.5), 1.5, 1.5);
      p.setPen(stroke);
      p.setBrush(Qt::NoBrush);
      f.moveTo(45, 59); f.quadTo(50, 63, 55, 59);
      p.drawPath(f);
      break;
    case Mood::kHot:
      f.moveTo(31, 42); f.lineTo(41, 47); f.lineTo(31, 52);
      f.moveTo(69, 42); f.lineTo(59, 47); f.lineTo(69, 52);
      f.moveTo(43, 61); f.quadTo(47, 56, 50, 61); f.quadTo(53, 66, 57, 61);
      p.drawPath(f);
      break;
    case Mood::kToasty:
      for (double ex : {36.0, 64.0}) {
        QPainterPath sp;
        for (double th = 0; th < 4 * M_PI; th += 0.3) {
          const double r = th * 0.42;
          const QPointF pt(ex + r * std::cos(th + t * 6), 46 + r * std::sin(th + t * 6));
          if (th == 0) sp.moveTo(pt); else sp.lineTo(pt);
        }
        p.drawPath(sp);
      }
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(0xff, 0x7a, 0x8a));
      p.drawEllipse(QPointF(50, 62), 5, 4);
      break;
    case Mood::kSleepy:
      f.moveTo(30, 47); f.quadTo(36, 52, 42, 47);
      f.moveTo(58, 47); f.quadTo(64, 52, 70, 47);
      p.drawPath(f);
      p.setPen(Qt::NoPen);
      p.setBrush(kInk);
      p.drawEllipse(QPointF(50, 61), 2.5, 2.5);
      QFont zf; zf.setPixelSize(14); zf.setBold(true);
      p.setFont(zf);
      p.setPen(kInkSoft);
      p.drawText(QPointF(72, 30 - std::fmod(t, 2.0) * 3), "z");
      break;
  }

  if (mood == Mood::kHot || mood == Mood::kToasty) {  // sweat drop
    const double ph = std::fmod(t, 1.4) / 1.4;
    QColor drop(0xbf, 0xe6, 0xff);
    drop.setAlphaF(std::sin(ph * M_PI));
    p.setPen(Qt::NoPen);
    p.setBrush(drop);
    p.translate(0, -2 + ph * 10);
    QPainterPath d;
    d.moveTo(82, 28); d.quadTo(88, 38, 82, 42); d.quadTo(76, 38, 82, 28);
    p.drawPath(d);
  }
  p.restore();
}


// App icon: the chill mascot on a soft pink squircle.
QImage renderIcon(int size) {
  QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
  img.fill(Qt::transparent);
  QPainter p(&img);
  p.setRenderHints(QPainter::Antialiasing);
  const double r = size * 0.225;
  QLinearGradient g(0, 0, size * 0.4, size);
  g.setColorAt(0, QColor(0xff, 0xe3, 0xee));
  g.setColorAt(1, QColor(0xff, 0xb8, 0xd2));
  p.setPen(Qt::NoPen);
  p.setBrush(g);
  p.drawRoundedRect(QRectF(size * 0.04, size * 0.04, size * 0.92, size * 0.92), r, r);
  paintMascot(p, size / 2.0, size * 0.5, size * 0.66, Mood::kChill, moodColor(Mood::kChill), 0.0, false);
  return img;
}

// ---- the window ---------------------------------------------------------
struct Button { double cx, cy, r; };

class Companion : public QWidget {
 public:
  Companion(bool snapshot_mode, double fake_temp)
      : fake_temp_(fake_temp), snapshot_mode_(snapshot_mode) {
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setWindowTitle("whyhot");
    setWindowIcon(QIcon(QPixmap::fromImage(renderIcon(256))));

    QSettings prefs("whyhot", "desktop");
    pinned_ = prefs.value("pinned", true).toBool();
    collapsed_ = prefs.value("collapsed", false).toBool();
    applyFlags();
    resizeForState();

    const QRect wa = screen()->availableGeometry();
    move(wa.right() - width() - 24, wa.top() + 24);

    store_ = std::make_unique<IncidentStore>(IncidentStore::defaultPath());
    engine_.setNextId(store_->maxKnownId() + 1);
    engine_.seedRecentClosed(store_->loadAll());

    clock_.start();
    connect_timers();
    sampleOnce();
  }

  void setCollapsed(bool c) {
    collapsed_ = c;
    resizeForState();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    const double t = clock_.elapsed() / 1000.0;
    const double dt = std::clamp(t - last_t_, 0.0, 0.2);
    last_t_ = t;

    const QColor tgt = moodColor(moodFor(view_));
    const double k = std::min(1.0, dt * 6.0);
    body_.setRgbF(body_.redF() + (tgt.redF() - body_.redF()) * k,
                  body_.greenF() + (tgt.greenF() - body_.greenF()) * k,
                  body_.blueF() + (tgt.blueF() - body_.blueF()) * k);
    const double rps = view_.fan > 0 ? std::clamp(view_.fan / 6000.0, 0.1, 1.0) * 1.2 : 0.1;
    fan_angle_ = std::fmod(fan_angle_ + dt * rps * 360.0, 360.0);

    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    drawCard(p);
    if (collapsed_) drawCollapsed(p, t); else drawExpanded(p, t);
    drawButtons(p);
  }

  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton) return;
    const QPointF pos = e->position();
    if (hit(closeBtn(), pos)) { QApplication::quit(); return; }
    if (hit(pinBtn(), pos)) { setPinned(!pinned_); return; }
    if (hit(foldBtn(), pos)) { toggleCollapsed(); return; }
    const bool in_header = collapsed_ || pos.y() < kMargin + 66;
    if (!in_header) return;
    if (pos.x() < kMargin + 64 && dbl_.isValid() && dbl_.elapsed() < 400) {
      dbl_.invalidate();
      toggleCollapsed();
      return;
    }
    dbl_.start();
    if (windowHandle()) windowHandle()->startSystemMove();
  }
  void mouseMoveEvent(QMouseEvent* e) override { mouse_ = e->position(); }
  void leaveEvent(QEvent*) override { mouse_ = QPointF(-1, -1); }

 private:
  CpuCollector cpu_;
  ProcessCollector process_{5};
  ThermalCollector thermal_;
  FanCollector fan_;
  PlatformCollector platform_;
  CorrelationEngine engine_;
  std::unique_ptr<IncidentStore> store_;
  View view_;

  double fake_temp_;
  bool snapshot_mode_;
  bool pinned_ = true, collapsed_ = false;
  QPointF mouse_{-1, -1};
  QElapsedTimer clock_, dbl_;
  double last_t_ = 0, fan_angle_ = 0;
  QColor body_{0xd9, 0xd4, 0xee};

  void connect_timers() {
    auto* frame = new QTimer(this);
    QObject::connect(frame, &QTimer::timeout, this, [this] {
      if (g_stop) QApplication::quit();
      update();
    });
    frame->start(40);  // ~25 fps
    auto* sample = new QTimer(this);
    QObject::connect(sample, &QTimer::timeout, this, [this] { sampleOnce(); });
    sample->start(1000);
  }

  void sampleOnce() {
    Sample s;
    s.ts = Clock::now();
    s.cpu = cpu_.sample();
    s.thermal = thermal_.sample();
    s.fan = fan_.sample();
    s.platform = platform_.sample();
    auto pr = process_.sample();
    s.top_processes = pr.top;
    // Snapshot mode is read-only so it can't pollute the real incident log.
    if (auto closed = engine_.addSample(s, pr.events); closed && !snapshot_mode_) store_->append(*closed);
    if (const Incident* opened = engine_.justOpened(); opened && !snapshot_mode_) store_->appendStart(*opened);
    view_ = buildView(engine_, fake_temp_);
  }

  void applyFlags() {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window |
                   (pinned_ ? Qt::WindowStaysOnTopHint : Qt::WindowType(0)));
  }
  void resizeForState() { setFixedSize(collapsed_ ? QSize(kCollapsedW, kCollapsedH) : QSize(kExpandedW, kExpandedH)); }

  void savePrefs() const {
    QSettings prefs("whyhot", "desktop");
    prefs.setValue("pinned", pinned_);
    prefs.setValue("collapsed", collapsed_);
  }
  void setPinned(bool on) {
    pinned_ = on;
    const QPoint pos = this->pos();
    applyFlags();  // changing flags hides the window
    show();
    move(pos);
    savePrefs();
  }
  void toggleCollapsed() {
    setCollapsed(!collapsed_);
    savePrefs();
  }

  Button pinBtn() const {
    return collapsed_ ? Button{kMargin + 226.0, kMargin + 14.0, 8} : Button{width() - kMargin - 74.0, kMargin + 32.0, 10};
  }
  Button foldBtn() const {
    return collapsed_ ? Button{kMargin + 226.0, kMargin + 32.0, 8} : Button{width() - kMargin - 48.0, kMargin + 32.0, 10};
  }
  Button closeBtn() const {
    return collapsed_ ? Button{kMargin + 226.0, kMargin + 50.0, 8} : Button{width() - kMargin - 22.0, kMargin + 32.0, 10};
  }
  static bool hit(const Button& b, QPointF p) {
    const double dx = p.x() - b.cx, dy = p.y() - b.cy;
    return dx * dx + dy * dy <= (b.r + 2) * (b.r + 2);
  }

  // ---- text ----
  QFont font(double px, bool bold) const {
    QFont f;
    f.setFamilies({"Nunito", "Quicksand", "Varela Round", "Comfortaa", "Noto Sans CJK KR", "Sans"});
    f.setPixelSize(static_cast<int>(px));
    f.setBold(bold);
    return f;
  }
  // align: 0 left, 1 center, 2 right. Returns the drawn width.
  double text(QPainter& p, const QString& s, double x, double y, double px, bool bold, const QColor& c,
              int align = 0) const {
    p.setFont(font(px, bold));
    p.setPen(c);
    const double w = QFontMetricsF(p.font()).horizontalAdvance(s);
    double ox = x;
    if (align == 1) ox = x - w / 2;
    if (align == 2) ox = x - w;
    p.drawText(QPointF(ox, y), s);
    return w;
  }
  QStringList wrap(const QString& s, double px, double maxw) const {
    const QFontMetricsF fm(font(px, true));
    QStringList lines;
    QString cur;
    for (const QString& word : s.split(' ', Qt::SkipEmptyParts)) {
      const QString t = cur.isEmpty() ? word : cur + " " + word;
      if (!cur.isEmpty() && fm.horizontalAdvance(t) > maxw) {
        lines << cur;
        cur = word;
      } else {
        cur = t;
      }
    }
    if (!cur.isEmpty()) lines << cur;
    return lines;
  }

  // ---- shapes ----
  static void fillRound(QPainter& p, QRectF r, double rad, const QColor& c) {
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(r, rad, rad);
  }

  void drawFlower(QPainter& p, double cx, double cy, double r, const QColor& c) const {
    p.save();
    p.translate(cx, cy);
    p.rotate(fan_angle_);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    for (int i = 0; i < 5; ++i) {
      p.save();
      p.rotate(i * 72.0);
      p.drawEllipse(QPointF(0, -r * 0.55), r * 0.38, r * 0.55);
      p.restore();
    }
    p.setBrush(QColor(0xff, 0xf3, 0xa8));
    p.drawEllipse(QPointF(0, 0), r * 0.25, r * 0.25);
    p.restore();
  }

  void drawMascot(QPainter& p, double cx, double cy, double size, Mood mood, double t) const {
    paintMascot(p, cx, cy, size, mood, body_, t, view_.incident);
  }

  void drawCard(QPainter& p) const {
    const QRectF r(kMargin, kMargin, width() - 2 * kMargin, height() - 2 * kMargin);
    const double rad = collapsed_ ? 24 : 28;
    for (int i = 8; i >= 1; --i)  // layered soft shadow
      fillRound(p, r.adjusted(-i + 1, -i + 4, i - 1, i + 2), rad + i, QColor(0xd6, 0x8f, 0xaf, 9));
    QLinearGradient g(0, r.top(), 0, r.bottom());
    g.setColorAt(0, QColor(0xff, 0xfa, 0xfc));
    g.setColorAt(1, QColor(0xff, 0xf0, 0xf6));
    p.setBrush(g);
    p.setPen(QPen(kLine, 2));
    p.drawRoundedRect(r, rad, rad);
  }

  void drawButtons(QPainter& p) const {
    const struct { Button b; int kind; } specs[] = {{pinBtn(), 0}, {foldBtn(), 1}, {closeBtn(), 2}};
    for (const auto& s : specs) {
      const bool hover = hit(s.b, mouse_);
      const double r = s.b.r * (hover ? 1.15 : 1.0);
      p.setPen(Qt::NoPen);
      p.setBrush(hover ? kLine : kPaper2);
      p.drawEllipse(QPointF(s.b.cx, s.b.cy), r, r);

      p.save();
      p.translate(s.b.cx, s.b.cy);
      p.scale(s.b.r / 10.0, s.b.r / 10.0);
      QPen pen(kInk, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
      if (s.kind == 0) {
        QColor c = pinned_ ? QColor(0xff, 0x7f, 0xa8) : kInkSoft;
        if (!pinned_) c.setAlphaF(0.6);
        pen.setColor(c);
        p.setPen(pen);
        p.setBrush(c);
        if (!pinned_) p.rotate(35);
        p.drawEllipse(QPointF(0, -2.5), 3.2, 3.2);
        p.drawLine(QPointF(0, 0), QPointF(0, 5));
      } else if (s.kind == 1) {
        p.setPen(pen);
        const double d = collapsed_ ? 1 : -1;  // chevron: up when expanded
        p.drawPolyline(QPolygonF({QPointF(-3.5, 1.5 * d), QPointF(0, -2 * d), QPointF(3.5, 1.5 * d)}));
      } else {
        p.setPen(pen);
        p.drawLine(QPointF(-3, -3), QPointF(3, 3));
        p.drawLine(QPointF(3, -3), QPointF(-3, 3));
      }
      p.restore();
    }
  }

  void drawCollapsed(QPainter& p, double t) const {
    const double x0 = kMargin, y0 = kMargin;
    drawMascot(p, x0 + 30, y0 + 33, 46, moodFor(view_), t);
    const struct { const char* label; QString value; QColor bg; } pills[3] = {
        {"temp", view_.temp >= 0 ? QString::number(view_.temp, 'f', 0) + "°" : "--", QColor(0xff, 0xe9, 0xe1)},
        {"fan", fanShort(view_.fan), QColor(0xe3, 0xf2, 0xff)},
        {"cpu", view_.have ? QString::number(view_.cpu, 'f', 0) + "%" : "--", QColor(0xef, 0xe8, 0xff)},
    };
    for (int i = 0; i < 3; ++i) {
      const double px = x0 + 58 + i * 52, py = y0 + 11;
      fillRound(p, {px, py, 48, 42}, 14, pills[i].bg);
      text(p, pills[i].label, px + 24, py + 14, 10, true, kInkSoft, 1);
      text(p, pills[i].value, px + 24, py + 34, 16, true, kInk, 1);
    }
    drawFlower(p, x0 + 58 + 52 + 40, y0 + 17, 4.5, QColor(0x7e, 0xc0, 0xf0));
  }

  void drawExpanded(QPainter& p, double t) const {
    const double x0 = kMargin, y0 = kMargin, cw = width() - 2 * kMargin, pad = 14, iw = cw - 2 * pad;
    drawMascot(p, x0 + 38, y0 + 34, 52, moodFor(view_), t);
    text(p, "whyhot", x0 + 72, y0 + 41, 23, true, kAccent);

    double y = y0 + 70;
    const QStringList lines = wrap(sayFor(view_, moodFor(view_)), 13, iw - 24);
    const double bh = std::max(44.0, lines.size() * 17.0 + 20);
    p.setPen(QPen(kLine, 2));
    p.setBrush(Qt::white);
    p.drawRoundedRect(QRectF(x0 + pad, y, iw, bh), 18, 18);
    QPainterPath tail;  // speech-bubble tail
    tail.moveTo(x0 + pad + 18, y + 1); tail.lineTo(x0 + pad + 28, y - 9); tail.lineTo(x0 + pad + 38, y + 1);
    p.drawPath(tail);
    p.setPen(QPen(Qt::white, 3.5));
    p.drawLine(QPointF(x0 + pad + 19.5, y + 1), QPointF(x0 + pad + 36.5, y + 1));
    for (int i = 0; i < lines.size(); ++i) text(p, lines[i], x0 + pad + 12, y + 22 + i * 17.0, 13, true, kInk);
    y += bh + 12;

    struct Stat { const char* label; QString val, unit; double frac; QColor bg, bar; };
    const Stat stats[3] = {
        {"temp", view_.temp >= 0 ? QString::number(view_.temp, 'f', 0) : "--", "°C", (view_.temp - 30) / 65.0,
         QColor(0xff, 0xea, 0xe2), QColor(0xff, 0xb1, 0x99)},
        {"fan", view_.fan >= 0 ? QString::number(view_.fan) : "--", "rpm", view_.fan / 6000.0,
         QColor(0xe6, 0xf3, 0xff), QColor(0xa9, 0xd8, 0xff)},
        {"cpu", view_.have ? QString::number(view_.cpu, 'f', 0) : "--", "%", view_.cpu / 100.0,
         QColor(0xf0, 0xea, 0xff), QColor(0xcd, 0xb9, 0xff)},
    };
    const double sw = (iw - 16) / 3.0;
    for (int i = 0; i < 3; ++i) {
      const double sx = x0 + pad + i * (sw + 8);
      fillRound(p, {sx, y, sw, 76}, 18, stats[i].bg);
      text(p, stats[i].label, sx + 10, y + 18, 11, true, kInkSoft);
      const double vw = text(p, stats[i].val, sx + 10, y + 44, 22, true, kInk);
      text(p, stats[i].unit, sx + 12 + vw, y + 44, 10, true, kInkSoft);
      const double bx = sx + 10, bw = sw - 20;
      fillRound(p, {bx, y + 55, bw, 7}, 3.5, QColor(255, 255, 255, 200));
      fillRound(p, {bx, y + 55, bw * std::clamp(stats[i].frac, 0.04, 1.0), 7}, 3.5, stats[i].bar);
    }
    drawFlower(p, x0 + pad + sw + 8 + sw - 14, y + 14, 6, QColor(0x7e, 0xc0, 0xf0));
    y += 76 + 20;

    text(p, "busiest friends", x0 + pad, y + 4, 16, true, kAccent);
    y += 12;
    if (view_.procs.empty()) {
      text(p, "nobody yet ☁", x0 + pad + 4, y + 18, 12, true, kInkSoft);
      y += 26;
    }
    for (const auto& pr : view_.procs) {
      fillRound(p, {x0 + pad, y, iw, 24}, 12, kPaper2);
      text(p, QString::fromStdString(pr.first), x0 + pad + 10, y + 16, 12, true, kInk);
      text(p, QString::number(pr.second, 'f', 0) + "%", x0 + pad + iw - 10, y + 16, 12, true, kAccent, 2);
      y += 28;
    }
    y += 14;

    text(p, "last little incident", x0 + pad, y + 4, 16, true, kAccent);
    y += 12;
    fillRound(p, {x0 + pad, y, iw, 62}, 14, kPaper2);
    if (view_.has_last) {
      text(p, QString::fromStdString(view_.last1), x0 + pad + 10, y + 17, 12, true, kInk);
      text(p, QString::fromStdString(view_.last2), x0 + pad + 10, y + 34, 12, true, kInk);
      text(p, QString::fromStdString(view_.last3), x0 + pad + 10, y + 51, 11, true, kInkSoft);
    } else {
      text(p, "nothing yet - all calm ☁", x0 + pad + 10, y + 36, 12, true, kInkSoft);
    }

    const QString foot = (view_.profile.empty() ? QString() : "mode: " + QString::fromStdString(view_.profile) + "  ·  ") +
                         "updated " + (view_.time.empty() ? QString("--") : QString::fromStdString(view_.time));
    text(p, foot, x0 + cw / 2, height() - kMargin - 10, 10, true, kInkSoft, 1);
  }
};

}  // namespace

int main(int argc, char** argv) {
  // Force XWayland/X11 so always-on-top works on GNOME/Wayland (unless the user overrides).
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "xcb");
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  QApplication app(argc, argv);
  app.setApplicationName("whyhot");

  // Hidden dev flags: --snapshot FILE [--collapsed] [--temp N] renders one
  // frame to a PNG (without touching the incident log or saved prefs) and exits.
  QString snapshot;
  for (int i = 1; i + 1 < argc; ++i) {  // --icon FILE [SIZE]: write the app icon PNG and exit
    if (std::string(argv[i]) == "--icon") {
      const int size = i + 2 < argc ? std::max(16, std::atoi(argv[i + 2])) : 256;
      return renderIcon(size).save(argv[i + 1]) ? 0 : 1;
    }
  }
  bool force_collapsed = false, force_expanded = false;
  double fake_temp = -1;
  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); ++i) {
    if (args[i] == "--snapshot" && i + 1 < args.size()) snapshot = args[++i];
    else if (args[i] == "--collapsed") force_collapsed = true;
    else if (args[i] == "--expanded") force_expanded = true;
    else if (args[i] == "--temp" && i + 1 < args.size()) fake_temp = args[++i].toDouble();
  }

  Companion w(!snapshot.isEmpty(), fake_temp);
  if (force_collapsed) w.setCollapsed(true);
  if (force_expanded) w.setCollapsed(false);
  w.show();

  if (!snapshot.isEmpty()) {
    QTimer::singleShot(2300, [&] {
      w.grab().save(snapshot);
      QApplication::quit();
    });
  }
  return app.exec();
}
