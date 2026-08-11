#include <QApplication>
#include <QScreen>
#include <QPixmap>
#include <QDebug>

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    auto screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); i++) {
        auto s = screens[i];
        QPixmap px = s->grabWindow(0);
        QString path = QString("K:/rv1126b_temp/s30gui_screen_%1.png").arg(i);
        bool ok = px.save(path);
        qDebug() << "screen" << i << s->name() << s->geometry() << "saved" << ok << px.size();
    }
    return 0;
}
