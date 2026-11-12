#include "theme.h"

#include <QApplication>
#include <QSettings>
#include <QMap>

namespace yasuo {
namespace Theme {

Palette palette(bool dark) {
    Palette p;
    if (dark) {
        p.page = QStringLiteral("#0F1218");
        p.card = QStringLiteral("#191E27");
        p.text = QStringLiteral("#E9ECF2");
        p.sub = QStringLiteral("#98A2B3");
        p.border = QStringLiteral("#2A303B");
        p.accent = QStringLiteral("#5B8CFF");
        p.accentHover = QStringLiteral("#7AA2FF");
        p.nav = QStringLiteral("#151A22");
        p.navActive = QStringLiteral("#22304A");
        p.danger = QStringLiteral("#FF6B72");
        p.success = QStringLiteral("#4ADE9B");
        p.soft = QStringLiteral("#212734");
        p.tableAlt = QStringLiteral("#1D232D");
    } else {
        p.page = QStringLiteral("#F2F4F9");
        p.card = QStringLiteral("#FFFFFF");
        p.text = QStringLiteral("#1B1F29");
        p.sub = QStringLiteral("#6B7280");
        p.border = QStringLiteral("#E4E8F0");
        p.accent = QStringLiteral("#2F6BFF");
        p.accentHover = QStringLiteral("#1E56E8");
        p.nav = QStringLiteral("#FFFFFF");
        p.navActive = QStringLiteral("#EAF0FF");
        p.danger = QStringLiteral("#E5484D");
        p.success = QStringLiteral("#18A96B");
        p.soft = QStringLiteral("#F4F6FB");
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
QFrame#card, QWidget#card { background: @CARD@; border: 1px solid @BORDER@; border-radius: 14px; }
QFrame#titleSep { background: @BORDER@; border: none; }
QLabel { color: @TEXT@; background: transparent; }
QLabel#subLabel { color: @SUB@; font-size: 12px; }
QLabel#titleLabel { font-size: 17px; font-weight: 600; }
QLabel#brandLabel { font-size: 16px; font-weight: 700; letter-spacing: 0.5px; }
QLabel#hintLabel { color: @SUB@; font-size: 11px; }
QLabel#brandLogo { background: transparent; }
QPushButton { background: @SOFT@; color: @TEXT@; border: 1px solid @BORDER@; border-radius: 9px; padding: 7px 14px; }
QPushButton:hover { background: @ACTIVE@; border-color: @ACCENT@; color: @ACCENT@; }
QPushButton:pressed { background: @ACTIVE@; }
QPushButton:disabled { color: @SUB@; border-color: @BORDER@; background: @SOFT@; }
QPushButton#primary { background: @ACCENT@; color: #FFFFFF; border: none; font-weight: 600; padding: 10px 20px; border-radius: 10px; }
QPushButton#primary:hover { background: @HOVER@; color: #FFFFFF; }
QPushButton#primary:disabled { background: @SUB@; color: #FFFFFF; }
QPushButton#danger { color: @DANGER@; }
QPushButton#navBtn { background: transparent; border: none; border-radius: 10px; padding: 10px 12px; text-align: left; color: @SUB@; font-size: 13px; }
QPushButton#navBtn:hover { background: @SOFT@; color: @TEXT@; }
QPushButton#navBtn:checked { background: @ACTIVE@; color: @ACCENT@; font-weight: 600; }
QPushButton#winBtn { background: transparent; border: none; border-radius: 8px; color: @SUB@; padding: 0; }
QPushButton#winBtn:hover { background: @SOFT@; color: @TEXT@; }
QPushButton#closeBtn { background: transparent; border: none; border-radius: 8px; color: @SUB@; padding: 0; }
QPushButton#closeBtn:hover { background: @DANGER@; color: #FFFFFF; }
QLineEdit, QComboBox, QSpinBox, QPlainTextEdit {
    background: @CARD@; color: @TEXT@; border: 1px solid @BORDER@; border-radius: 8px; padding: 7px 10px;
    selection-background-color: @ACCENT@; selection-color: #FFFFFF;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QPlainTextEdit:focus { border-color: @ACCENT@; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView { background: @CARD@; color: @TEXT@; border: 1px solid @BORDER@;
    selection-background-color: @ACTIVE@; selection-color: @ACCENT@; outline: 0; }
QListWidget, QTreeView, QTableView {
    background: @CARD@; color: @TEXT@; border: 1px solid @BORDER@; border-radius: 10px;
    alternate-background-color: @ALT@; outline: 0; padding: 4px;
}
QListView::item, QTreeView::item { padding: 6px 6px; border-radius: 6px; }
QListView::item:hover, QTreeView::item:hover { background: @SOFT@; }
QListView::item:selected, QTreeView::item:selected { background: @ACTIVE@; color: @ACCENT@; }
QHeaderView::section { background: @SOFT@; color: @SUB@; border: none; border-bottom: 1px solid @BORDER@;
    padding: 8px 8px; font-weight: 600; }
QProgressBar { background: @SOFT@; border: none; border-radius: 7px; height: 12px; text-align: center; color: @TEXT@; font-size: 11px; }
QProgressBar::chunk { background: @ACCENT@; border-radius: 7px; }
QGroupBox { border: 1px solid @BORDER@; border-radius: 12px; margin-top: 16px; padding-top: 14px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 6px; color: @SUB@; }
QCheckBox, QRadioButton { color: @TEXT@; spacing: 8px; }
QSlider::groove:horizontal { height: 6px; background: @SOFT@; border-radius: 3px; }
QSlider::handle:horizontal { width: 16px; height: 16px; background: @ACCENT@; border-radius: 8px; margin: -5px 0; }
QSlider::sub-page:horizontal { background: @ACCENT@; border-radius: 3px; }
QToolTip { background: @CARD@; color: @TEXT@; border: 1px solid @BORDER@; padding: 6px 8px; border-radius: 6px; }
QStatusBar { background: @CARD@; border-top: 1px solid @BORDER@; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: @BORDER@; border-radius: 5px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: @SUB@; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: @BORDER@; border-radius: 5px; min-width: 24px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
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
