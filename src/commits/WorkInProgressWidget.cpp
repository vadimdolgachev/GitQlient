#include <WorkInProgressWidget.h>
#include <ui_WorkInProgressWidget.h>

#include <GitRepoLoader.h>
#include <GitBase.h>
#include <GitLocal.h>
#include <GitQlientStyles.h>
#include <CommitInfo.h>
#include <RevisionFiles.h>
#include <UnstagedMenu.h>
#include <RevisionsCache.h>
#include <FileWidget.h>

#include <QDir>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QRegExp>
#include <QScrollBar>
#include <QTextCodec>
#include <QToolTip>
#include <QListWidgetItem>
#include <QTextStream>
#include <QProcess>
#include <QItemDelegate>
#include <QPainter>

#include <QLogger.h>

using namespace QLogger;

const int WorkInProgressWidget::kMaxTitleChars = 50;

QString WorkInProgressWidget::lastMsgBeforeError;

enum GitQlientRole
{
   U_ListRole = Qt::UserRole,
   U_IsConflict,
   U_Name
};

WorkInProgressWidget::WorkInProgressWidget(const QSharedPointer<RevisionsCache> &cache,
                                           const QSharedPointer<GitBase> &git, QWidget *parent)
   : QWidget(parent)
   , ui(new Ui::WorkInProgressWidget)
   , mCache(cache)
   , mGit(git)
{
   ui->setupUi(this);
   setAttribute(Qt::WA_DeleteOnClose);

   ui->lCounter->setText(QString::number(kMaxTitleChars));
   ui->leCommitTitle->setMaxLength(kMaxTitleChars);
   ui->teDescription->setMaximumHeight(125);

   QIcon stagedIcon(":/icons/staged");
   ui->stagedFilesIcon->setPixmap(stagedIcon.pixmap(15, 15));

   QIcon unstagedIcon(":/icons/unstaged");
   ui->unstagedIcon->setPixmap(unstagedIcon.pixmap(15, 15));

   QIcon untrackedIcon(":/icons/untracked");
   ui->untrackedFilesIcon->setPixmap(untrackedIcon.pixmap(15, 15));

   connect(ui->leCommitTitle, &QLineEdit::textChanged, this, &WorkInProgressWidget::updateCounter);
   connect(ui->leCommitTitle, &QLineEdit::returnPressed, this, &WorkInProgressWidget::commitChanges);
   connect(ui->pbCommit, &QPushButton::clicked, this, &WorkInProgressWidget::commitChanges);
   connect(ui->untrackedFilesList, &UntrackedFilesList::signalShowDiff, this, &WorkInProgressWidget::requestDiff);
   connect(ui->untrackedFilesList, &UntrackedFilesList::signalStageFile, this,
           &WorkInProgressWidget::addFileToCommitList);
   connect(ui->untrackedFilesList, &UntrackedFilesList::signalCheckoutPerformed, this,
           &WorkInProgressWidget::signalCheckoutPerformed);
   connect(ui->stagedFilesList, &StagedFilesList::signalResetFile, this, &WorkInProgressWidget::resetFile);
   connect(ui->stagedFilesList, &StagedFilesList::signalShowDiff, this, &WorkInProgressWidget::requestDiff);
   connect(ui->unstagedFilesList, &QListWidget::customContextMenuRequested, this,
           &WorkInProgressWidget::showUnstagedMenu);
   connect(ui->unstagedFilesList, &QListWidget::itemDoubleClicked, this,
           [this](QListWidgetItem *item) { requestDiff(item->toolTip()); });

   ui->pbCancelAmend->setVisible(false);
   ui->leAuthorName->setVisible(false);
   ui->leAuthorEmail->setVisible(false);
   ui->pbCommit->setText(tr("Commit"));
}

WorkInProgressWidget::~WorkInProgressWidget()
{
   delete ui;
}

void WorkInProgressWidget::configure(const QString &sha)
{
   const auto commit = mCache->getCommitInfo(sha);

   if (commit.parentsCount() <= 0)
      return;

   if (!mCache->containsRevisionFile(CommitInfo::ZERO_SHA, commit.parent(0)))
   {
      QScopedPointer<GitRepoLoader> git(new GitRepoLoader(mGit, mCache));
      git->updateWipRevision();
   }

   const auto files = mCache->getRevisionFile(CommitInfo::ZERO_SHA, commit.parent(0));

   QLog_Info("UI", QString("Updating files for SHA {%1}").arg(mCurrentSha));

   prepareCache();

   insertFiles(files, ui->unstagedFilesList);

   clearCache();

   ui->lUnstagedCount->setText(QString("(%1)").arg(ui->unstagedFilesList->count()));
   ui->lStagedCount->setText(QString("(%1)").arg(ui->stagedFilesList->count()));
   ui->teDescription->moveCursor(QTextCursor::Start);
   ui->pbCommit->setEnabled(ui->stagedFilesList->count());
}

void WorkInProgressWidget::resetFile(QListWidgetItem *item)
{
   QScopedPointer<GitLocal> git(new GitLocal(mGit));
   const auto ret = git->resetFile(item->toolTip());
   const auto revInfo = mCache->getCommitInfo(mCurrentSha);
   const auto files = mCache->getRevisionFile(mCurrentSha, revInfo.parent(0));

   for (auto i = 0; i < files.count(); ++i)
   {
      auto fileName = files.getFile(i);

      if (fileName == item->toolTip())
      {
         const auto isUnknown = files.statusCmp(i, RevisionFiles::UNKNOWN);
         const auto isInIndex = files.statusCmp(i, RevisionFiles::IN_INDEX);
         const auto untrackedFile = !isInIndex && isUnknown;
         const auto row = ui->stagedFilesList->row(item);
         const auto iconPath = QString(":/icons/add");

         if (isInIndex)
         {
            item->setData(GitQlientRole::U_ListRole, QVariant::fromValue(ui->unstagedFilesList));

            ui->stagedFilesList->takeItem(row);
            ui->unstagedFilesList->addItem(item);

            const auto fileWidget = new FileWidget(iconPath, item->toolTip());
            fileWidget->setTextColor(item->foreground().color());

            connect(fileWidget, &FileWidget::clicked, this, [this, item]() { addFileToCommitList(item); });
            ui->unstagedFilesList->setItemWidget(item, fileWidget);
         }
         else if (untrackedFile)
         {
            item->setData(GitQlientRole::U_ListRole, QVariant::fromValue(ui->untrackedFilesList));

            ui->stagedFilesList->takeItem(row);
            ui->untrackedFilesList->addItem(item);

            const auto fileWidget = new FileWidget(iconPath, item->toolTip());
            fileWidget->setTextColor(item->foreground().color());

            connect(fileWidget, &FileWidget::clicked, this, [this, item]() { addFileToCommitList(item); });
            ui->untrackedFilesList->setItemWidget(item, fileWidget);
         }
      }
   }

   if (ret.success)
      emit signalUpdateWip();
}

QColor WorkInProgressWidget::getColorForFile(const RevisionFiles &files, int index) const
{
   const auto isUnknown = files.statusCmp(index, RevisionFiles::UNKNOWN);
   const auto isInIndex = files.statusCmp(index, RevisionFiles::IN_INDEX);
   const auto isConflict = files.statusCmp(index, RevisionFiles::CONFLICT);
   const auto untrackedFile = !isInIndex && isUnknown;

   QColor myColor;
   const auto isDeleted = files.statusCmp(index, RevisionFiles::DELETED);

   if (isConflict)
   {
      myColor = GitQlientStyles::getBlue();
   }
   else if (isDeleted)
      myColor = GitQlientStyles::getRed();
   else if (untrackedFile)
      myColor = GitQlientStyles::getOrange();
   else if (files.statusCmp(index, RevisionFiles::NEW) || isUnknown || isInIndex)
      myColor = GitQlientStyles::getGreen();
   else
      myColor = GitQlientStyles::getTextColor();

   return myColor;
}

void WorkInProgressWidget::prepareCache()
{
   for (auto file = mCurrentFilesCache.begin(); file != mCurrentFilesCache.end(); ++file)
      file.value().first = false;
}

void WorkInProgressWidget::clearCache()
{
   for (auto it = mCurrentFilesCache.begin(); it != mCurrentFilesCache.end();)
   {
      if (!it.value().first)
      {
         delete it.value().second;
         it = mCurrentFilesCache.erase(it);
      }
      else
         ++it;
   }
}

void WorkInProgressWidget::insertFiles(const RevisionFiles &files, QListWidget *fileList)
{
   for (auto i = 0; i < files.count(); ++i)
   {
      const auto fileName = files.getFile(i);

      if (!mCurrentFilesCache.contains(fileName))
      {
         const auto isUnknown = files.statusCmp(i, RevisionFiles::UNKNOWN);
         const auto isInIndex = files.statusCmp(i, RevisionFiles::IN_INDEX);
         const auto isConflict = files.statusCmp(i, RevisionFiles::CONFLICT);
         const auto untrackedFile = !isInIndex && isUnknown;
         const auto staged = isInIndex && !isUnknown && !isConflict;

         auto parent = fileList;

         if (untrackedFile)
            parent = ui->untrackedFilesList;
         else if (staged)
            parent = ui->stagedFilesList;

         auto item = new QListWidgetItem(fileList);
         item->setData(GitQlientRole::U_ListRole, QVariant::fromValue(parent));
         item->setData(GitQlientRole::U_Name, fileName);

         if (fileList == ui->stagedFilesList)
            item->setFlags(item->flags() & (~Qt::ItemIsSelectable & ~Qt::ItemIsEnabled));

         mCurrentFilesCache.insert(fileName, qMakePair(true, item));

         if (isConflict)
         {
            item->setData(GitQlientRole::U_IsConflict, isConflict);

            const auto newName = fileName + QString(" (conflicts");
            item->setText(newName);
            item->setData(GitQlientRole::U_Name, newName);
         }
         else
            item->setText(fileName);

         item->setToolTip(fileName);

         const auto fileWidget = new FileWidget(
             !untrackedFile && staged ? QString(":/icons/remove") : QString(":/icons/add"), item->text());
         const auto textColor = getColorForFile(files, i);

         fileWidget->setTextColor(textColor);
         item->setForeground(textColor);

         if (staged)
            connect(fileWidget, &FileWidget::clicked, this, [this, item]() { resetFile(item); });
         else
            connect(fileWidget, &FileWidget::clicked, this, [this, item]() { addFileToCommitList(item); });

         fileList->setItemWidget(item, fileWidget);
         item->setText("");
         item->setSizeHint(fileWidget->sizeHint());
      }
      else
         mCurrentFilesCache[fileName].first = true;
   }
}

void WorkInProgressWidget::addAllFilesToCommitList()
{
   for (auto i = ui->unstagedFilesList->count() - 1; i >= 0; --i)
      addFileToCommitList(ui->unstagedFilesList->item(i));

   ui->lUnstagedCount->setText(QString("(%1)").arg(ui->unstagedFilesList->count()));
   ui->lStagedCount->setText(QString("(%1)").arg(ui->stagedFilesList->count()));
   ui->pbCommit->setEnabled(ui->stagedFilesList->count() > 0);
}

void WorkInProgressWidget::requestDiff(const QString &fileName)
{
   emit signalShowDiff(CommitInfo::ZERO_SHA, mCache->getCommitInfo(CommitInfo::ZERO_SHA).parent(0), fileName);
}

void WorkInProgressWidget::addFileToCommitList(QListWidgetItem *item)
{
   const auto fileList = qvariant_cast<QListWidget *>(item->data(GitQlientRole::U_ListRole));
   const auto row = fileList->row(item);
   const auto fileWidget = qobject_cast<FileWidget *>(fileList->itemWidget(item));
   const auto newFileWidget = new FileWidget(":/icons/remove", fileWidget->text());
   newFileWidget->setTextColor(item->foreground().color());

   connect(newFileWidget, &FileWidget::clicked, this, [this, item]() { removeFileFromCommitList(item); });

   fileList->removeItemWidget(item);
   fileList->takeItem(row);

   ui->stagedFilesList->addItem(item);
   ui->stagedFilesList->setItemWidget(item, newFileWidget);

   if (item->data(GitQlientRole::U_IsConflict).toBool())
      newFileWidget->setText(newFileWidget->text().remove("(conflicts)").trimmed());

   delete fileWidget;

   ui->lUntrackedCount->setText(QString("(%1)").arg(ui->untrackedFilesList->count()));
   ui->lUnstagedCount->setText(QString("(%1)").arg(ui->unstagedFilesList->count()));
   ui->lStagedCount->setText(QString("(%1)").arg(ui->stagedFilesList->count()));
   ui->pbCommit->setEnabled(true);
}

void WorkInProgressWidget::revertAllChanges()
{
   auto needsUpdate = false;

   for (auto i = ui->unstagedFilesList->count() - 1; i >= 0; --i)
   {
      QScopedPointer<GitLocal> git(new GitLocal(mGit));
      needsUpdate |= git->checkoutFile(ui->unstagedFilesList->takeItem(i)->toolTip());
   }

   if (needsUpdate)
      emit signalCheckoutPerformed();
}

void WorkInProgressWidget::removeFileFromCommitList(QListWidgetItem *item)
{
   if (item->flags() & Qt::ItemIsSelectable)
   {
      const auto itemOriginalList = qvariant_cast<QListWidget *>(item->data(GitQlientRole::U_ListRole));
      const auto row = ui->stagedFilesList->row(item);
      const auto fileWidget = qobject_cast<FileWidget *>(ui->stagedFilesList->itemWidget(item));
      const auto newFileWidget = new FileWidget(":/icons/add", fileWidget->text());
      newFileWidget->setTextColor(item->foreground().color());

      connect(newFileWidget, &FileWidget::clicked, this, [this, item]() { addFileToCommitList(item); });

      if (item->data(GitQlientRole::U_IsConflict).toBool())
         newFileWidget->setText(fileWidget->text().append(" (conflicts)"));

      delete fileWidget;

      ui->stagedFilesList->removeItemWidget(item);
      const auto item = ui->stagedFilesList->takeItem(row);

      itemOriginalList->addItem(item);
      itemOriginalList->setItemWidget(item, newFileWidget);

      ui->lUnstagedCount->setText(QString("(%1)").arg(ui->unstagedFilesList->count()));
      ui->lStagedCount->setText(QString("(%1)").arg(ui->stagedFilesList->count()));
      ui->pbCommit->setDisabled(ui->stagedFilesList->count() == 0);
   }
}

QStringList WorkInProgressWidget::getFiles()
{
   QStringList selFiles;
   const auto totalItems = ui->stagedFilesList->count();

   for (auto i = 0; i < totalItems; ++i)
   {
      const auto fileWidget = static_cast<FileWidget *>(ui->stagedFilesList->itemWidget(ui->stagedFilesList->item(i)));
      selFiles.append(fileWidget->text());
   }

   return selFiles;
}

bool WorkInProgressWidget::checkMsg(QString &msg)
{
   const auto title = ui->leCommitTitle->text();

   if (title.isEmpty())
      QMessageBox::warning(this, "Commit changes", "Please, add a title.");

   msg = title;

   if (!ui->teDescription->toPlainText().isEmpty())
   {
      auto description = QString("\n\n%1").arg(ui->teDescription->toPlainText());
      description.remove(QRegExp("(^|\\n)\\s*#[^\\n]*")); // strip comments
      msg += description;
   }

   msg.replace(QRegExp("[ \\t\\r\\f\\v]+\\n"), "\n"); // strip line trailing cruft
   msg = msg.trimmed();

   if (msg.isEmpty())
   {
      QMessageBox::warning(this, "Commit changes", "Please, add a title.");
      return false;
   }

   QString subj(msg.section('\n', 0, 0, QString::SectionIncludeTrailingSep));
   QString body(msg.section('\n', 1).trimmed());
   msg = subj + '\n' + body + '\n';

   return true;
}

void WorkInProgressWidget::updateCounter(const QString &text)
{
   ui->lCounter->setText(QString::number(kMaxTitleChars - text.count()));
}

bool WorkInProgressWidget::hasConflicts()
{
   for (const auto &pair : mCurrentFilesCache.values())
      if (pair.second->data(GitQlientRole::U_IsConflict).toBool())
         return true;

   return false;
}

bool WorkInProgressWidget::commitChanges()
{
   QString msg;
   QStringList selFiles = getFiles();
   auto done = false;

   if (!selFiles.isEmpty())
   {
      if (hasConflicts())
         QMessageBox::warning(this, tr("Impossible to commit"),
                              tr("There are files with conflicts. Please, resolve the conflicts first."));
      else if (checkMsg(msg))
      {
         const auto revInfo = mCache->getCommitInfo(CommitInfo::ZERO_SHA);
         QScopedPointer<GitRepoLoader> gitLoader(new GitRepoLoader(mGit, mCache));
         gitLoader->updateWipRevision();
         const auto files = mCache->getRevisionFile(CommitInfo::ZERO_SHA, revInfo.parent(0));

         QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
         QScopedPointer<GitLocal> git(new GitLocal(mGit));
         const auto ret = git->commitFiles(selFiles, files, msg);
         QApplication::restoreOverrideCursor();

         lastMsgBeforeError = (ret.success ? "" : msg);

         emit signalChangesCommitted(ret.success);

         done = true;

         ui->leCommitTitle->clear();
         ui->teDescription->clear();
      }
   }

   return done;
}

void WorkInProgressWidget::clear()
{
   ui->untrackedFilesList->clear();
   ui->unstagedFilesList->clear();
   ui->stagedFilesList->clear();
   mCurrentFilesCache.clear();
   ui->leCommitTitle->clear();
   ui->teDescription->clear();
   ui->pbCommit->setEnabled(false);
   ui->lStagedCount->setText(QString("(%1)").arg(ui->unstagedFilesList->count()));
   ui->lUnstagedCount->setText(QString("(%1)").arg(ui->stagedFilesList->count()));
   ui->lUntrackedCount->setText(QString("(%1)").arg(ui->unstagedFilesList->count()));
}

void WorkInProgressWidget::showUnstagedMenu(const QPoint &pos)
{
   const auto item = ui->unstagedFilesList->itemAt(pos);

   if (item)
   {
      const auto fileName = item->toolTip();
      const auto unsolvedConflicts = item->data(GitQlientRole::U_IsConflict).toBool();
      const auto contextMenu = new UnstagedMenu(mGit, fileName, unsolvedConflicts, this);
      connect(contextMenu, &UnstagedMenu::signalEditFile, this,
              [this, fileName]() { emit signalEditFile(mGit->getWorkingDir() + "/" + fileName, 0, 0); });
      connect(contextMenu, &UnstagedMenu::signalShowDiff, this, &WorkInProgressWidget::requestDiff);
      connect(contextMenu, &UnstagedMenu::signalCommitAll, this, &WorkInProgressWidget::addAllFilesToCommitList);
      connect(contextMenu, &UnstagedMenu::signalRevertAll, this, &WorkInProgressWidget::revertAllChanges);
      connect(contextMenu, &UnstagedMenu::signalCheckedOut, this, &WorkInProgressWidget::signalCheckoutPerformed);
      connect(contextMenu, &UnstagedMenu::signalShowFileHistory, this, &WorkInProgressWidget::signalShowFileHistory);
      connect(contextMenu, &UnstagedMenu::signalStageFile, this, [this, item] { addFileToCommitList(item); });
      connect(contextMenu, &UnstagedMenu::signalConflictsResolved, this, [this, item] {
         const auto fileWidget = qobject_cast<FileWidget *>(ui->unstagedFilesList->itemWidget(item));

         item->setData(GitQlientRole::U_IsConflict, false);
         item->setText(fileWidget->text().remove("(conflicts)").trimmed());
         item->setForeground(GitQlientStyles::getGreen());
         configure(mCurrentSha);
      });

      const auto parentPos = ui->unstagedFilesList->mapToParent(pos);
      contextMenu->popup(mapToGlobal(parentPos));
   }
}
