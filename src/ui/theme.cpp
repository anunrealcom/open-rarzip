#include "theme.h"

#include <QApplication>
#include <QSettings>
#include <QMap>

namespace yasuo {
namespace Theme {

Palette palette(bool dark) {
    Palette p;
    if (dark) {
        p.page = QStringLiteral("#12151A");
        p.card = QStringLiteral("#1C2029");
        p.text = QStringLiteral("#E9ECF2");
        p.sub = QStringLiteral("#98A0AF");
        p.border = QStringLiteral("#2C323D");
        p.accent = QStringLiteral("#4C86FF");
        p.accentHover = QStringLiteral("#6A9BFF");
        p.nav = QStringLiteral("#171B22");
        p.navActive = QStringLiteral("#26344E");
        p.danger = QStringLiteral("#FF6B72");
        p.success = QStringLiteral("#4ADE9B");
        p.soft = QStringLiteral("#232833");
        p.tableAlt = QStringLiteral("#20252F");
    } else {
        p.page = QStringLiteral("#F4F6FA");
        p.card = QStringLiteral("#FFFFFF");
        p.text = QStringLiteral("#1B1F29");
        p.sub = QStringLiteral("#6B7280");
        p.border = QStringLiteral("#E3E7EF");
        p.accent = QStringLiteral("#2F6BFF");
        p.accentHover = QStringLiteral("#1E56E8");
        p.nav = QStringLiteral("#FFFFFF");
        p.navActive = QStringLiteral("#E9F0FF");
        p.danger = QStringLiteral("#E5484D");
        p.success = QStringLiteral("#18A96B");
        p.soft = QStringLiteral("#F5F7FB");
        p.tableAlt = QStringLiteral("#FAFBFE");
    }
    return p;
}

bool isDark() {
    QSettings s;
    return s.value(QStringLiteral("ui/dark"), false).toBool();
}

void setDark(bool dark) {
    QSettings s;
    s.setValue(QStringLiteral("ui/dark"), dark);
}

void apply(bool dark) {
    if (qApp) qApp->setStyleSheet(styleSheet(dark));
}

QString styleSheet(bool dark) {
    const Palette p = palette(dark);
    QString css = QStringLiteral(R"(
QWidget { font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif; font-size: 13px; color: @TEXT@; }
QMainWindow, QWidget#pageRoot { background: @PAGE@; }
QFrame#card, QWidget#card { background: @CARD@; border: 1px solid @BORDER@; border-radius: 12px; }
QLabel { color: @TEXT@; background: transparent; }
QLabel#subLabel { color: @SUB@; font-size: 12px; }
QLabel#titleLabel { font-size: 16px; font-weight: 600; }
QLabel#brandLabel { font-size: 15px; font-weight: 600; letter-spacing: 1px; }
QLabel#hintLabel { color: @SUB@; font-size: 11px; }
QPushButton { background: @SOFT@; color: @TEXT@; border: 1px solid @BORDER@; border-radius: 8px; padding: 7px 14px; }
QPushButton:hover { border-color: @ACCENT@; color: @ACCENT@; }
QPushButton:pressed { background: @ACTIVE@; }
QPushButton:disabled { color: @SUB@; border-color: @BORDER@; background: @SOFT@; }
QPushButton#primary { background: @ACCENT@; color: #FFFFFF; border: none; font-weight: 600; padding: 10px 18px; }
QPushButton#primary:hover { background: @HOVER@; }
QPushButton#primary:disabled { background: @SUB@; color: #FFFFFF; }
QPushButton#danger { color: @DANGER@; }
QPushButton#navBtn { background: transparent; border: none; border-radius: 9px; padding: 10px 8px; text-align: left; color: @SUB@; font-size: 12px; }
QPushButton#navBtn:hover { background: @SOFT@; color: @TEXT@; }
QPushButton#navBtn:checked { background: @ACTIVE@; color: @ACCENT@; font-weight: 600; }
QPushButton#winBtn { background: transparent; border: none; border-radius: 6px; color: @SUB@; padding: 0; }
QPushButton#winBtn:hover { background: @SOFT@; color: @TEXT@; }
QPushButton#closeBtn:hover { background: @DANGER@; color: #FFFFFF; }
QLineEdit, QComboBox, QSpinBox, QPlainTextEdit {
    background: @CARD@; color: @TEXT@; border: 1px solid @BORDER@; border-radius: 8px; padding: 7px 10px;
    selection-background-color: @ACCENT@; selection-color: #FFFFFF;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: @ACCENT@; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView { background: @CARD@; color: @TEXT@; border: 1px solid @BORDER@;
    selection-background-color: @ACTIVE@; selection-color: @ACCENT@; outline: 0; }
QListWidget, QTreeView, QTableView {
    background: @CARD@; color: @TEXT@; border: 1px solid @BORDER@; border-radius: 10px;
    alternate-background-color: @ALT@; outline: 0; padding: 4px;
}
QListView::item, QTreeView::item { padding: 5px 4px; border-radius: 6px; }
QListView::item:hover, QTreeView::item:hover { background: @SOFT@; }
QListView::item:selected, QTreeView::item:selected { background: @ACTIVE@; color: @ACCENT@; }
QHeaderView::section { background: @SOFT@; color: @SUB@; border: none; border-bottom: 1px solid @BORDER@;
    padding: 7px 8px; font-weight: 600; }
QProgressBar { background: @SOFT@; border: none; border-radius: 6px; height: 10px; text-align: center; color: @TEXT@; font-size: 11px; }
QProgressBar::chunk { background: @ACCENT@; border-radius: 6px; }
QGroupBox { border: 1px solid @BORDER@; border-radius: 10px; margin-top: 14px; padding-top: 12px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; color: @SUB@; }
QCheckBox, QRadioButton { color: @TEXT@; spacing: 7px; }
QSlider::groove:horizontal { height: 5px; background: @SOFT@; border-radius: 3px; }
QSlider::handle:horizontal { width: 15px; height: 15px; background: @ACCENT@; border-radius: 8px; margin: -5px 0; }
QSlider::sub-page:horizontal { background: @ACCENT@; border-radius: 3px; }
QToolTip { background: @CARD@; color: @TEXT@; border: 1px solid @BORDER@; padding: 5px; }
QScrollBar:vertical { background: transparent; width: 9px; margin: 0; }
QScrollBar::handle:vertical { background: @BORDER@; border-radius: 5px; min-height: 24px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)");
    const QMap<QString, QString> tokens = {
        { QStringLiteral("@TEXT@"),   p.text },
        { QStringLiteral("@PAGE@"),   p.page },
        { QStringLiteral("@CARD@"),   p.card },
        { QStringLiteral("@SUB@"),    p.sub },
        { QStringLiteral("@BORDER@"), p.border },
        { QStringLiteral("@ACCENT@"), p.accent },
        { QStringLiteral("@HOVER@"),  p.accentHover },
        { QStringLiteral("@DANGER@"), p.danger },
        { QStringLiteral("@ACTIVE@"), p.navActive },
        { QStringLiteral("@SOFT@"),   p.soft },
        { QStringLiteral("@ALT@"),    p.tableAlt }
    };
    for (auto it = tokens.constBegin(); it != tokens.constEnd(); ++it)
        css.replace(it.key(), it.value());
    return css;
}

} // namespace Theme
} // namespace yasuo
