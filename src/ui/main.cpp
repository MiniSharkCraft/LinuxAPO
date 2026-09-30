#include "MainWindow.h"

#include <QApplication>

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("SkyAPO");
  QCoreApplication::setApplicationName("SkyAPO");
  MainWindow window(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString{});
  window.show();
  return app.exec();
}
