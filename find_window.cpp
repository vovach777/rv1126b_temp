#include <QApplication>
#include <QScreen>
#include <QPixmap>
#include <QDebug>
#include <QWindow>
#include <QGuiApplication>

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    // Найти все окна
    auto windows = QGuiApplication::topLevelWindows();
    qDebug() << "Top-level windows:" << windows.size();
    for (auto w : windows) {
        qDebug() << "  " << w->title() << w->geometry() << "visible=" << w->isVisible();
    }
    return 0;
}
