#include "mainwindow.h"
#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("YasuoCompressor"));
    QApplication::setOrganizationName(QStringLiteral("Yasuo"));

    MainWindow w;
    w.show();
    return app.exec();
}
