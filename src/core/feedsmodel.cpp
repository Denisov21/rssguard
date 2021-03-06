// This file is part of RSS Guard.
//
// Copyright (C) 2011-2016 by Martin Rotter <rotter.martinos@gmail.com>
//
// RSS Guard is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// RSS Guard is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with RSS Guard. If not, see <http://www.gnu.org/licenses/>.

#include "core/feedsmodel.h"

#include "definitions/definitions.h"
#include "services/abstract/feed.h"
#include "services/abstract/category.h"
#include "services/abstract/serviceroot.h"
#include "services/abstract/recyclebin.h"
#include "services/abstract/serviceentrypoint.h"
#include "services/standard/standardserviceroot.h"
#include "miscellaneous/textfactory.h"
#include "miscellaneous/databasefactory.h"
#include "miscellaneous/iconfactory.h"
#include "miscellaneous/feedreader.h"

#include <QSqlError>
#include <QSqlRecord>
#include <QPair>
#include <QStack>
#include <QMimeData>

#include <algorithm>


FeedsModel::FeedsModel(QObject *parent) : QAbstractItemModel(parent) {
  setObjectName(QSL("FeedsModel"));

  // Create root item.
  m_rootItem = new RootItem();

  //: Name of root item of feed list which can be seen in feed add/edit dialog.
  m_rootItem->setTitle(tr("Root"));
  m_rootItem->setIcon(qApp->icons()->fromTheme(QSL("folder")));

  // Setup icons.
  m_countsIcon = qApp->icons()->fromTheme(QSL("mail-mark-unread"));

  //: Title text in the feed list header.
  m_headerData << tr("Title");

  m_tooltipData << /*: Feed list header "titles" column tooltip.*/ tr("Titles of feeds/categories.") <<
                   /*: Feed list header "counts" column tooltip.*/ tr("Counts of unread/all mesages.");
}

FeedsModel::~FeedsModel() {
  qDebug("Destroying FeedsModel instance.");

  foreach (ServiceRoot *account, serviceRoots()) {
    account->stop();
  }

  // Delete all model items.
  delete m_rootItem;
}

QMimeData *FeedsModel::mimeData(const QModelIndexList &indexes) const {
  QMimeData *mime_data = new QMimeData();
  QByteArray encoded_data;
  QDataStream stream(&encoded_data, QIODevice::WriteOnly);

  foreach (const QModelIndex &index, indexes) {
    if (index.column() != 0) {
      continue;
    }

    RootItem *item_for_index = itemForIndex(index);

    if (item_for_index->kind() != RootItemKind::Root) {
      stream << (quintptr) item_for_index;
    }
  }

  mime_data->setData(MIME_TYPE_ITEM_POINTER, encoded_data);
  return mime_data;
}

QStringList FeedsModel::mimeTypes() const {
  return QStringList() << MIME_TYPE_ITEM_POINTER;
}

bool FeedsModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) {
  Q_UNUSED(row)
  Q_UNUSED(column)

  if (action == Qt::IgnoreAction) {
    return true;
  }
  else if (action != Qt::MoveAction) {
    return false;
  }

  QByteArray dragged_items_data = data->data(MIME_TYPE_ITEM_POINTER);

  if (dragged_items_data.isEmpty()) {
    return false;
  }
  else {
    QDataStream stream(&dragged_items_data, QIODevice::ReadOnly);

    while (!stream.atEnd()) {
      quintptr pointer_to_item;
      stream >> pointer_to_item;

      // We have item we want to drag, we also determine the target item.
      RootItem *dragged_item = (RootItem*) pointer_to_item;
      RootItem *target_item = itemForIndex(parent);
      ServiceRoot *dragged_item_root = dragged_item->getParentServiceRoot();
      ServiceRoot *target_item_root = target_item->getParentServiceRoot();

      if (dragged_item == target_item || dragged_item->parent() == target_item) {
        qDebug("Dragged item is equal to target item or its parent is equal to target item. Cancelling drag-drop action.");
        return false;
      }

      if (dragged_item_root != target_item_root) {
        // Transferring of items between different accounts is not possible.
        qApp->showGuiMessage(tr("Cannot perform drag & drop operation"),
                             tr("You can't transfer dragged item into different account, this is not supported."),
                             QSystemTrayIcon::Warning,
                             qApp->mainFormWidget(),
                             true);

        qDebug("Dragged item cannot be dragged into different account. Cancelling drag-drop action.");
        return false;
      }

      if (dragged_item->performDragDropChange(target_item)) {
        // Drag & drop is supported by the dragged item and was
        // completed on data level and in item hierarchy.
        emit requireItemValidationAfterDragDrop(indexForItem(dragged_item));
      }
    }

    return true;
  }

  return false;
}

Qt::DropActions FeedsModel::supportedDropActions() const {
  return Qt::MoveAction;
}

Qt::ItemFlags FeedsModel::flags(const QModelIndex &index) const {
  Qt::ItemFlags base_flags = QAbstractItemModel::flags(index);
  const RootItem *item_for_index = itemForIndex(index);
  Qt::ItemFlags additional_flags = item_for_index->additionalFlags();

  return base_flags | additional_flags;
}

QVariant FeedsModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal) {
    return QVariant();
  }

  switch (role) {
    case Qt::DisplayRole:
      if (section == FDS_MODEL_TITLE_INDEX) {
        return m_headerData.at(FDS_MODEL_TITLE_INDEX);
      }
      else {
        return QVariant();
      }

    case Qt::ToolTipRole:
      return m_tooltipData.at(section);

    case Qt::DecorationRole:
      if (section == FDS_MODEL_COUNTS_INDEX) {
        return m_countsIcon;
      }
      else {
        return QVariant();
      }

    default:
      return QVariant();
  }
}

QModelIndex FeedsModel::index(int row, int column, const QModelIndex &parent) const {
  if (!hasIndex(row, column, parent)) {
    return QModelIndex();
  }

  RootItem *parent_item = itemForIndex(parent);
  RootItem *child_item = parent_item->child(row);

  if (child_item) {
    return createIndex(row, column, child_item);
  }
  else {
    return QModelIndex();
  }
}

QModelIndex FeedsModel::parent(const QModelIndex &child) const {
  if (!child.isValid()) {
    return QModelIndex();
  }

  RootItem *child_item = itemForIndex(child);
  RootItem *parent_item = child_item->parent();

  if (parent_item == m_rootItem) {
    return QModelIndex();
  }
  else {
    return createIndex(parent_item->row(), 0, parent_item);
  }
}

int FeedsModel::rowCount(const QModelIndex &parent) const {
  if (parent.column() > 0) {
    return 0;
  }
  else {
    return itemForIndex(parent)->childCount();
  }
}

int FeedsModel::countOfAllMessages() const {
  return m_rootItem->countOfAllMessages();
}

int FeedsModel::countOfUnreadMessages() const {
  return m_rootItem->countOfUnreadMessages();
}

void FeedsModel::reloadCountsOfWholeModel() {
  m_rootItem->updateCounts(true);
  reloadWholeLayout();
  notifyWithCounts();
}

void FeedsModel::removeItem(const QModelIndex &index) {
  if (index.isValid()) {
    RootItem *deleting_item = itemForIndex(index);
    QModelIndex parent_index = index.parent();
    RootItem *parent_item = deleting_item->parent();

    beginRemoveRows(parent_index, index.row(), index.row());
    parent_item->removeChild(deleting_item);
    endRemoveRows();

    deleting_item->deleteLater();
    notifyWithCounts();
  }
}

void FeedsModel::removeItem(RootItem *deleting_item) {
  if (deleting_item != nullptr) {
    QModelIndex index = indexForItem(deleting_item);
    QModelIndex parent_index = index.parent();
    RootItem *parent_item = deleting_item->parent();

    beginRemoveRows(parent_index, index.row(), index.row());
    parent_item->removeChild(deleting_item);
    endRemoveRows();

    deleting_item->deleteLater();
    notifyWithCounts();
  }
}

void FeedsModel::reassignNodeToNewParent(RootItem *original_node, RootItem *new_parent) {
  RootItem *original_parent = original_node->parent();

  if (original_parent != new_parent) {
    if (original_parent != nullptr) {
      int original_index_of_item = original_parent->childItems().indexOf(original_node);

      if (original_index_of_item >= 0) {
        // Remove the original item from the model...
        beginRemoveRows(indexForItem(original_parent), original_index_of_item, original_index_of_item);
        original_parent->removeChild(original_node);
        endRemoveRows();
      }
    }

    int new_index_of_item = new_parent->childCount();

    // ... and insert it under the new parent.
    beginInsertRows(indexForItem(new_parent), new_index_of_item, new_index_of_item);
    new_parent->appendChild(original_node);
    endInsertRows();
  }
}

QList<ServiceRoot*> FeedsModel::serviceRoots() const {
  QList<ServiceRoot*> roots;

  foreach (RootItem *root, m_rootItem->childItems()) {
    if (root->kind() == RootItemKind::ServiceRoot) {
      roots.append(root->toServiceRoot());
    }
  }

  return roots;
}

bool FeedsModel::containsServiceRootFromEntryPoint(const ServiceEntryPoint *point) const {
  foreach (const ServiceRoot *root, serviceRoots()) {
    if (root->code() == point->code()) {
      return true;
    }
  }

  return false;
}

StandardServiceRoot *FeedsModel::standardServiceRoot() const {
  foreach (ServiceRoot *root, serviceRoots()) {
    StandardServiceRoot *std_service_root;

    if ((std_service_root = dynamic_cast<StandardServiceRoot*>(root)) != nullptr) {
      return std_service_root;
    }
  }

  return nullptr;
}

QList<Feed*> FeedsModel::feedsForScheduledUpdate(bool auto_update_now) {
  QList<Feed*> feeds_for_update;

  foreach (Feed *feed, m_rootItem->getSubTreeFeeds()) {
    switch (feed->autoUpdateType()) {
      case Feed::DontAutoUpdate:
        // Do not auto-update this feed ever.
        continue;

      case Feed::DefaultAutoUpdate:
        if (auto_update_now) {
          feeds_for_update.append(feed);
        }

        break;

      case Feed::SpecificAutoUpdate:
      default:
        int remaining_interval = feed->autoUpdateRemainingInterval();

        if (--remaining_interval <= 0) {
          // Interval of this feed passed, include this feed in the output list
          // and reset the interval.
          feeds_for_update.append(feed);
          feed->setAutoUpdateRemainingInterval(feed->autoUpdateInitialInterval());
        }
        else {
          // Interval did not pass, set new decremented interval and do NOT
          // include this feed in the output list.
          feed->setAutoUpdateRemainingInterval(remaining_interval);
        }

        break;
    }
  }

  return feeds_for_update;
}

QList<Message> FeedsModel::messagesForItem(RootItem *item) const {
  return item->undeletedMessages();
}

int FeedsModel::columnCount(const QModelIndex &parent) const {
  Q_UNUSED(parent)

  return FEEDS_VIEW_COLUMN_COUNT;
}

RootItem *FeedsModel::itemForIndex(const QModelIndex &index) const {
  if (index.isValid() && index.model() == this) {
    return static_cast<RootItem*>(index.internalPointer());
  }
  else {
    return m_rootItem;
  }
}

QModelIndex FeedsModel::indexForItem(const RootItem *item) const {
  if (item == nullptr || item->kind() == RootItemKind::Root) {
    // Root item lies on invalid index.
    return QModelIndex();
  }

  QStack<const RootItem*> chain;

  while (item->kind() != RootItemKind::Root) {
    chain.push(item);
    item = item->parent();
  }

  // Now, we have complete chain list: parent --- ..... --- parent --- leaf (item).
  QModelIndex target_index = indexForItem(m_rootItem);

  // We go through the stack and create our target index.
  while (!chain.isEmpty()) {
    const RootItem *parent_item = chain.pop();
    target_index = index(parent_item->parent()->childItems().indexOf(const_cast<RootItem* const>(parent_item)), 0, target_index);
  }

  return target_index;
}

bool FeedsModel::hasAnyFeedNewMessages() const {
  foreach (const Feed *feed, m_rootItem->getSubTreeFeeds()) {
    if (feed->status() == Feed::NewMessages) {
      return true;
    }
  }

  return false;
}

RootItem *FeedsModel::rootItem() const {
  return m_rootItem;
}

void FeedsModel::reloadChangedLayout(QModelIndexList list) {
  while (!list.isEmpty()) {
    QModelIndex indx = list.takeFirst();

    if (indx.isValid()) {
      QModelIndex indx_parent = indx.parent();

      // Underlying data are changed.
      emit dataChanged(index(indx.row(), 0, indx_parent), index(indx.row(), FDS_MODEL_COUNTS_INDEX, indx_parent));

      list.append(indx_parent);
    }
  }
}

void FeedsModel::reloadChangedItem(RootItem *item) {
  QModelIndex index_item = indexForItem(item);
  reloadChangedLayout(QModelIndexList() << index_item);
}

void FeedsModel::notifyWithCounts() {
  emit messageCountsChanged(countOfUnreadMessages(), hasAnyFeedNewMessages());
}

void FeedsModel::onItemDataChanged(const QList<RootItem *> &items) {
  if (items.size() > RELOAD_MODEL_BORDER_NUM) {
    qDebug("There is request to reload feed model for more than %d items, reloading model fully.", RELOAD_MODEL_BORDER_NUM);
    reloadWholeLayout();
  }
  else {
    qDebug("There is request to reload feed model, reloading the %d items individually.", items.size());

    foreach (RootItem *item, items) {
      reloadChangedItem(item);
    }
  }

  notifyWithCounts();
}

void FeedsModel::reloadWholeLayout() {
  emit layoutAboutToBeChanged();
  emit layoutChanged();
}

bool FeedsModel::addServiceAccount(ServiceRoot *root, bool freshly_activated) {
  int new_row_index = m_rootItem->childCount();

  beginInsertRows(indexForItem(m_rootItem), new_row_index, new_row_index);
  m_rootItem->appendChild(root);
  endInsertRows();

  // Connect.
  connect(root, SIGNAL(itemRemovalRequested(RootItem*)), this, SLOT(removeItem(RootItem*)));
  connect(root, SIGNAL(itemReassignmentRequested(RootItem*,RootItem*)), this, SLOT(reassignNodeToNewParent(RootItem*,RootItem*)));
  connect(root, SIGNAL(dataChanged(QList<RootItem*>)), this, SLOT(onItemDataChanged(QList<RootItem*>)));
  connect(root, SIGNAL(reloadMessageListRequested(bool)), this, SIGNAL(reloadMessageListRequested(bool)));
  connect(root, SIGNAL(itemExpandRequested(QList<RootItem*>,bool)), this, SIGNAL(itemExpandRequested(QList<RootItem*>,bool)));
  connect(root, SIGNAL(itemExpandStateSaveRequested(RootItem*)), this, SIGNAL(itemExpandStateSaveRequested(RootItem*)));

  root->start(freshly_activated);
  return true;
}

bool FeedsModel::restoreAllBins() {
  bool result = true;

  foreach (ServiceRoot *root, serviceRoots()) {
    RecycleBin *bin_of_root = root->recycleBin();

    if (bin_of_root != nullptr) {
      result &= bin_of_root->restore();
    }
  }

  return result;
}

bool FeedsModel::emptyAllBins() {
  bool result = true;

  foreach (ServiceRoot *root, serviceRoots()) {
    RecycleBin *bin_of_root = root->recycleBin();

    if (bin_of_root != nullptr) {
      result &= bin_of_root->empty();
    }
  }

  return result;
}

void FeedsModel::loadActivatedServiceAccounts() {
  // Iterate all globally available feed "service plugins".
  foreach (const ServiceEntryPoint *entry_point, qApp->feedReader()->feedServices()) {
    // Load all stored root nodes from the entry point and add those to the model.
    QList<ServiceRoot*> roots = entry_point->initializeSubtree();

    foreach (ServiceRoot *root, roots) {
      addServiceAccount(root, false);
    }
  }
}

QList<Feed*> FeedsModel::feedsForIndex(const QModelIndex &index) const {
  return itemForIndex(index)->getSubTreeFeeds();
}

bool FeedsModel::markItemRead(RootItem *item, RootItem::ReadStatus read) {
  return item->markAsReadUnread(read);
}

bool FeedsModel::markItemCleared(RootItem *item, bool clean_read_only) {
  return item->cleanMessages(clean_read_only);
}
