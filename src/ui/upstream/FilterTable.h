#pragma once

// Narrow Linux-facing adapter for the upstream FilterTableRow widget. The
// original FilterTable owns Windows APO devices, registry preferences and a
// large set of Windows-only directive factories; SkyAPO supplies only the
// row operations used by the reusable upstream row.
#include <QKeyEvent>
#include <QMenu>
#include <QMetaType>
#include <QMouseEvent>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>
#include <QVariantMap>
#include <QWidget>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "Editor/FilterTemplate.h"
#include "Editor/IFilterGUI.h"

class FilterTable : public QWidget {
  Q_OBJECT
public:
  struct Item {
    QString text;
    QVariantMap prefs;
    IFilterGUI *gui{};
    int index{};
    QPointer<QWidget> row;
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
  // The upstream ResizeCorner calls this legacy FilterTable sizing helper.
  void setMinimumHeightHint(int height) { setMinimumHeight(height); }
  const QSet<Item *> &getSelectedItems() const { return selected; }
  Item *getFocusedItem() const { return focused; }
  Item *itemAt(int index) const {
    return index >= 0 && static_cast<size_t>(index) < orderedItems.size()
               ? orderedItems[static_cast<size_t>(index)]
               : nullptr;
  }
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
    orderedItems.clear();
    selected.clear();
    focused = nullptr;
    while (layout->count() > 1) {
      auto *item = layout->takeAt(0);
      if (item->widget())
        item->widget()->deleteLater();
      delete item;
    }
  }
  void addRow(QWidget *row, Item *item) {
    row->setProperty("skyapoFilterItem", QVariant::fromValue<quintptr>(
                                             reinterpret_cast<quintptr>(item)));
    item->row = row;
    orderedItems.push_back(item);
    installOnChildren(row);
    layout->insertWidget(layout->count() - 1, row);
  }
  void setSelection(Item *item, bool additive = false, bool range = false) {
    if (!item)
      return;
    if (!additive && !range)
      selected.clear();
    if (range && focused) {
      auto first = std::find(orderedItems.begin(), orderedItems.end(), focused);
      auto last = std::find(orderedItems.begin(), orderedItems.end(), item);
      if (first != orderedItems.end() && last != orderedItems.end()) {
        selected.clear();
        if (first > last)
          std::swap(first, last);
        for (auto it = first; it <= last; ++it)
          selected.insert(*it);
      }
    } else if (additive && selected.contains(item)) {
      selected.remove(item);
    } else {
      selected.insert(item);
    }
    focused = item;
    updateRows();
  }
  void
  setHandlers(std::function<void(QMenu *)> popup,
              std::function<void(const QString &, Item *)> add,
              std::function<void(Item *)> remove, std::function<void()> rebuild,
              std::function<void()> model,
              std::function<void(const std::vector<Item *>)> removeSelection,
              std::function<void(Item *, int)> move) {
    popupFactory = std::move(popup);
    addHandler = std::move(add);
    removeHandler = std::move(remove);
    rebuildHandler = std::move(rebuild);
    modelHandler = std::move(model);
    removeSelectionHandler = std::move(removeSelection);
    moveHandler = std::move(move);
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() == QEvent::MouseButtonPress) {
      QWidget *widget = qobject_cast<QWidget *>(watched);
      while (widget && widget != this) {
        const QVariant value = widget->property("skyapoFilterItem");
        if (value.isValid()) {
          auto *item = reinterpret_cast<Item *>(value.value<quintptr>());
          const auto *mouse = static_cast<QMouseEvent *>(event);
          const auto modifiers = mouse->modifiers();
          setSelection(item, modifiers.testFlag(Qt::ControlModifier),
                       modifiers.testFlag(Qt::ShiftModifier));
          break;
        }
        widget = widget->parentWidget();
      }
    } else if (event->type() == QEvent::KeyPress) {
      const auto *key = static_cast<QKeyEvent *>(event);
      if (key->key() == Qt::Key_Delete && !selected.isEmpty()) {
        std::vector<Item *> items(selected.begin(), selected.end());
        std::sort(items.begin(), items.end(),
                  [](Item *a, Item *b) { return a->index > b->index; });
        if (removeSelectionHandler)
          removeSelectionHandler(items);
        return true;
      }
      if (key->modifiers().testFlag(Qt::AltModifier) && focused &&
          (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
        if (moveHandler)
          moveHandler(focused, key->key() == Qt::Key_Up ? -1 : 1);
        return true;
      }
      if (key->key() == Qt::Key_Escape && !selected.isEmpty()) {
        selected.clear();
        focused = nullptr;
        updateRows();
        return true;
      }
    }
    return QWidget::eventFilter(watched, event);
  }

private:
  void installOnChildren(QWidget *widget) {
    widget->installEventFilter(this);
    for (auto *child : widget->findChildren<QWidget *>())
      child->installEventFilter(this);
  }
  void updateRows() {
    for (auto *item : orderedItems)
      if (item->row)
        item->row->update();
  }

  QVBoxLayout *layout{};
  QMenu *popupMenu{};
  QSet<Item *> selected;
  Item *focused{};
  std::function<void(QMenu *)> popupFactory;
  std::function<void(const QString &, Item *)> addHandler;
  std::function<void(Item *)> removeHandler;
  std::function<void()> rebuildHandler;
  std::function<void()> modelHandler;
  std::function<void(const std::vector<Item *>)> removeSelectionHandler;
  std::function<void(Item *, int)> moveHandler;
  std::vector<Item *> orderedItems;
};
