#pragma once

// Narrow Linux-facing adapter for the upstream FilterTableRow widget. The
// original FilterTable owns Windows APO devices, registry preferences and a
// large set of Windows-only directive factories; SkyAPO supplies only the
// row operations used by the reusable upstream row.
#include <QMenu>
#include <QMetaType>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>
#include <QVariantMap>
#include <QWidget>
#include <functional>

#include "Editor/FilterTemplate.h"
#include "Editor/IFilterGUI.h"

class FilterTable : public QWidget {
public:
  struct Item {
    QString text;
    QVariantMap prefs;
    IFilterGUI *gui{};
    int index{};
  };

  explicit FilterTable(QWidget *parent = nullptr) : QWidget(parent) {
    popupMenu = new QMenu(this);
    popupMenu->setObjectName(QStringLiteral("upstreamFilterAddMenu"));
    layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(5);
    layout->addStretch(1);
  }

  int getPreferredWidth() const { return width(); }
  const QSet<Item *> &getSelectedItems() const { return selected; }
  Item *getFocusedItem() const { return focused; }
  QMenu *createAddPopupMenu() {
    popupMenu->clear();
    if (popupFactory)
      popupFactory(popupMenu);
    return popupMenu;
  }
  void addLine(const QString &line, Item *before = nullptr) {
    if (addHandler)
      addHandler(line, before);
  }
  void removeItem(Item *item) {
    if (removeHandler)
      removeHandler(item);
  }
  void updateGuis() {
    if (rebuildHandler)
      rebuildHandler();
  }
  void updateModel() {
    if (modelHandler)
      modelHandler();
  }
  void clearRows() {
    while (layout->count() > 1) {
      auto *item = layout->takeAt(0);
      if (item->widget())
        item->widget()->deleteLater();
      delete item;
    }
  }
  void addRow(QWidget *row) { layout->insertWidget(layout->count() - 1, row); }
  void setHandlers(std::function<void(QMenu *)> popup,
                   std::function<void(const QString &, Item *)> add,
                   std::function<void(Item *)> remove,
                   std::function<void()> rebuild, std::function<void()> model) {
    popupFactory = std::move(popup);
    addHandler = std::move(add);
    removeHandler = std::move(remove);
    rebuildHandler = std::move(rebuild);
    modelHandler = std::move(model);
  }

private:
  QVBoxLayout *layout{};
  QMenu *popupMenu{};
  QSet<Item *> selected;
  Item *focused{};
  std::function<void(QMenu *)> popupFactory;
  std::function<void(const QString &, Item *)> addHandler;
  std::function<void(Item *)> removeHandler;
  std::function<void()> rebuildHandler;
  std::function<void()> modelHandler;
};
