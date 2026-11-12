#pragma once

#include <QColor>
#include <QIcon>

namespace yasuo {

// 界面统一图标集：全部由 QPainter 在 24x24 逻辑坐标系内绘制，
// 无需外部图片资源，按传入颜色自动适配深/浅主题，并按 2x 渲染适配高分屏。
enum class IconKind {
    AddFile, AddFolder, Remove, Clear, Browse, Start, Cancel, Check,
    NavCompress, NavArchive, NavTask, NavSettings,
    Sun, Moon, Minimize, Maximize, Restore, Close
};

namespace Icons {
QIcon make(IconKind kind, const QColor& color, int size = 18);
}

} // namespace yasuo
