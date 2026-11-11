#pragma once

#include <QString>

namespace yasuo {
namespace Theme {

bool isDark();
void setDark(bool dark);
void apply(bool dark);
QString styleSheet(bool dark);

struct Palette {
    QString page, card, text, sub, border, accent, accentHover,
            nav, navActive, danger, success, soft, tableAlt;
};
Palette palette(bool dark);

} // namespace Theme
} // namespace yasuo
