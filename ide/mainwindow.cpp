/*
 * This file is part of Embedded-IDE
 * 
 * Copyright 2019 Martin Ribelotta <martinribelotta@gmail.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "mainwindow.h"

#include "ui_mainwindow.h"

#include "appconfig.h"
#include "buildmanager.h"
#include "consoleinterceptor.h"
#include "filesystemmanager.h"
#include "idocumenteditor.h"
#include "externaltoolmanager.h"
#include "processmanager.h"
#include "projectmanager.h"
#include "unsavedfilesdialog.h"
#include "version.h"
#include "newprojectdialog.h"
#include "configwidget.h"
#include "findinfilesdialog.h"
#include "clangautocompletionprovider.h"
#include "textmessagebrocker.h"
#include "regexhtmltranslator.h"
#include "templatemanager.h"
#include "templateitemwidget.h"
#include "templatefile.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QStringListModel>
#include <QScrollBar>
#include <QMenu>
#include <QMessageBox>
#include <QFileSystemModel>
#include <QShortcut>
#include <QStandardItemModel>
#include <QFileSystemWatcher>
#include <QTextBrowser>
#include <QDialogButtonBox>

#include <QtDebug>

class MainWindow::Priv_t {
public:
    ProjectManager *projectManager;
    FileSystemManager *fileManager;
    ProcessManager *pman;
    ConsoleInterceptor *console;
    BuildManager *buildManager;
};

static constexpr auto MainWindowSIZE = QSize{900, 600};

MainWindow::MainWindow(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::MainWindow),
    priv(new Priv_t)
{
    ui->setupUi(this);
    ui->stackedWidget->setCurrentWidget(ui->welcomePage);
    ui->bottomLeftStack->setCurrentWidget(ui->actionViewer);

    auto loadIcons = [this]() {
#define _(b, name) ui->b->setIcon(QIcon{AppConfig::resourceImage({ "actions", name })})
    _(buttonDocumentCloseAll, "document-close-all");
    _(buttonReload, "view-refresh");
    _(buttonCloseProject, "document-close");
    _(buttonExport, "document-export");
    _(buttonFindAll, "edit-find-replace");
    _(buttonTools, "run-build-configure");
    _(buttonConfigurationMain, "configure");
    _(buttonDebugLaunch, "debug-init");
    _(buttonDebugRun, "debug-run-v2");
    _(buttonDebugStepOver, "debug-step-over-v2");
    _(buttonDebugStepInto, "debug-step-into-v2");
    _(buttonDebugRunToEOF, "debug-step-out-v2");
    _(buttonDocumentClose, "document-close");
    _(buttonDocumentSave, "document-save");
    _(buttonDocumentCloseAll, "document-close-all");
    _(buttonDocumentSaveAll, "document-save-all");
    _(buttonDocumentReload, "view-refresh");
    _(updateAvailable, "view-refresh");
    _(buttonQuit, "application-exit");
    _(buttonConfiguration, "configure");
    // _(buttonExternalTools, "run-build-configure");
    _(buttonOpenProject, "document-open");
    _(buttonNewProject, "document-new");
#undef _
    };
    loadIcons();
    connect(&AppConfig::instance(), &AppConfig::configChanged, loadIcons);

    ui->buttonReload->setIcon(QIcon(AppConfig::resourceImage({"actions", "view-refresh"})));

    ui->buttonDebugLaunch->setVisible(AppConfig::instance().useDevelopMode());
    ui->updateAvailable->setVisible(false);

    auto tman = new TemplateManager();
    tman->setRepositoryUrl(AppConfig::instance().templatesUrl());
    connect(tman, &TemplateManager::message, [](const QString& msg) {
        qDebug() << "tman msg:" << msg;
    });
    connect(tman, &TemplateManager::errorMessage, [](const QString& msg) {
        qDebug() << "tman err:" << msg;
    });
    connect(tman, &TemplateManager::haveMetadata, [this, tman]() {
        bool canUpdate = false;
        auto list = tman->itemWidgets();
        for(auto *witem: list) {
            const auto& item = witem->templateItem();
            canUpdate = canUpdate ||
                (item.state() == TemplateItem::State::Updatable) ||
                (item.state() == TemplateItem::State::New);
        }
        ui->updateAvailable->setVisible(canUpdate);
    });
    connect(ui->updateAvailable, &QToolButton::clicked, [this, tman]() {
        static constexpr auto SIZE_GROW = 0.9;
        QDialog d(this);
        d.resize(this->size().scaled(this->size() * SIZE_GROW, Qt::KeepAspectRatio));
        auto l = new QVBoxLayout(&d);
        auto bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
        l->addWidget(tman);
        l->addWidget(bb);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        d.exec();
        ui->updateAvailable->hide();
        tman->setParent(nullptr);
        tman->startUpdate();
    });
    tman->startUpdate();


    ui->stackedWidget->setCurrentWidget(ui->welcomePage);
    ui->documentContainer->setComboBox(ui->documentSelector);
    auto version = tr("%1 build at %2").arg(VERSION, BUILD_DATE);
    ui->labelVersion->setText(ui->labelVersion->text().replace("{{version}}", version));
    resize(MainWindowSIZE);

    priv->pman = new ProcessManager(this);
    priv->console = new ConsoleInterceptor(ui->logView, priv->pman, BuildManager::PROCESS_NAME, this);
    priv->console->addStdErrFilter(RegexHTMLTranslator::CONSOLE_TRANSLATOR);
    priv->projectManager = new ProjectManager(ui->actionViewer, priv->pman, this);
    priv->buildManager = new BuildManager(priv->projectManager, priv->pman, this);
    priv->fileManager = new FileSystemManager(ui->fileViewer, this);
    ui->documentContainer->setProjectManager(priv->projectManager);
    priv->projectManager->setCodeModelProvider(new ClangAutocompletionProvider(priv->projectManager, this));

    connect(ui->logView, &QTextBrowser::anchorClicked, [this](const QUrl& url) {
        auto path = url.path();
        auto lr = url.fragment().split('#');
        auto ok1 = false;
        auto ok2 = false;
        constexpr auto DEC_RADIX = 10;
        int line = !lr.empty()? lr.at(0).toInt(&ok1, DEC_RADIX) : 1;
        int chdr = lr.size()>1? lr.at(1).toInt(&ok2, DEC_RADIX) : 1;
        if (!ok1) line = 1;
        if (!ok2) chdr = 1;
        ui->documentContainer->openDocumentHere(path, line, chdr);
        ui->documentContainer->setFocus();
    });

    TextMessageBrocker::instance().subscribe(TextMessages::STDERR_LOG, [this](const QString& msg) {
        priv->console->writeHtml(msg);
    });

    TextMessageBrocker::instance().subscribe(TextMessages::STDOUT_LOG, [this](const QString& msg) {
        priv->console->writeHtml(msg);
    });

    connect(priv->buildManager, &BuildManager::buildStarted, [this]() { ui->actionViewer->setEnabled(false); });
    connect(priv->buildManager, &BuildManager::buildTerminated, [this]() { ui->actionViewer->setEnabled(true); });
    connect(priv->projectManager, &ProjectManager::targetTriggered, [this](const QString& target) {
        ui->logView->clear();
        auto unsaved = ui->documentContainer->unsavedDocuments();
        if (!unsaved.isEmpty()) {
            UnsavedFilesDialog d(unsaved, this);
            if (d.exec() == QDialog::Rejected)
                return;
            ui->documentContainer->saveDocuments(d.checkedForSave());
        }
        priv->buildManager->startBuild(target);
    });
    connect(priv->fileManager, &FileSystemManager::requestFileOpen, ui->documentContainer, &DocumentManager::openDocument);

    auto showMessageCallback = [this](const QString& msg) { priv->console->writeMessage(msg, Qt::darkGreen); };
    auto clearMessageCallback = [this]() { ui->logView->clear(); };
    connect(priv->projectManager, &ProjectManager::exportFinish, showMessageCallback);

    ui->recentProjectsView->setModel(new QStandardItemModel(ui->recentProjectsView));
    auto makeRecentProjects = [this]() {
        auto m = dynamic_cast<QStandardItemModel*>(ui->recentProjectsView->model());
        m->clear();
        for(const auto& e: AppConfig::instance().recentProjects()) {
            auto item = new QStandardItem(
                QIcon(FileSystemManager::mimeIconPath("text-x-makefile")), e.dir().dirName());
            item->setData(e.absoluteFilePath());
            item->setToolTip(e.absoluteFilePath());
            m->appendRow(item);
        }
    };
    makeRecentProjects();
    connect(new QFileSystemWatcher({AppConfig::instance().projectsPath()}, this),
            &QFileSystemWatcher::directoryChanged, makeRecentProjects);
    connect(ui->recentProjectsView, &QListView::activated, [this](const QModelIndex& m) {
        openProject(dynamic_cast<const QStandardItemModel*>(m.model())->data(m, Qt::UserRole + 1).toString());
    });

    auto openProjectCallback = [this]() {
        auto lastDir = property("lastDir").toString();
        if (lastDir.isEmpty())
            lastDir = AppConfig::instance().projectsPath();
        auto path = QFileDialog::getOpenFileName(this, tr("Open Project"), lastDir, tr("Makefile (Makefile);;All files (*)"));
        if (!path.isEmpty()) {
            openProject(path);
            setProperty("lastDir", QFileInfo(QFileInfo(path).absolutePath()).absolutePath());
        }
    };
    auto newProjectCallback = [this]() {
        NewProjectDialog d(this);
        if (d.exec() == QDialog::Accepted) {
            if (d.isTemplate()) {
                priv->projectManager->createProject(d.absoluteProjectPath(), d.templateFile());
            } else {
                priv->projectManager->createProjectFromTGZ(d.absoluteProjectPath(),
                                                           d.selectedTemplateFile().absoluteFilePath());
            }
        }
    };
    auto openConfigurationCallback = [this]() {
        ConfigWidget d(this);
        if (d.exec())
            d.save();
    };
    auto exportCallback = [this, clearMessageCallback]() {
        auto path = QFileDialog::getSaveFileName(this, tr("New File"), AppConfig::instance().templatesPath(),
                                                 TemplateFile::TEMPLATE_FILEDIALOG_FILTER);
        if (!path.isEmpty()) {
            if (QFileInfo(path).suffix().isEmpty())
                path.append(".template");
            clearMessageCallback();
            priv->projectManager->exportCurrentProjectTo(path);
        }
    };
    connect(ui->buttonOpenProject, &QToolButton::clicked, openProjectCallback);
    connect(ui->buttonExport, &QToolButton::clicked, exportCallback);
    connect(ui->buttonNewProject, &QToolButton::clicked, newProjectCallback);
    connect(ui->buttonConfiguration, &QToolButton::clicked, openConfigurationCallback);
    connect(ui->buttonConfigurationMain, &QToolButton::clicked, openConfigurationCallback);
    connect(ui->buttonCloseProject, &QToolButton::clicked, priv->projectManager, &ProjectManager::closeProject);
    connect(new QShortcut(QKeySequence("CTRL+N"), this), &QShortcut::activated, newProjectCallback);
    connect(new QShortcut(QKeySequence("CTRL+O"), this), &QShortcut::activated, openProjectCallback);
    connect(new QShortcut(QKeySequence("CTRL+SHIFT+P"), this), &QShortcut::activated, openConfigurationCallback);
    connect(new QShortcut(QKeySequence("CTRL+SHIFT+Q"), this), &QShortcut::activated, priv->projectManager, &ProjectManager::closeProject);

    setProperty("documentOnly", false);
    connect(new QShortcut(QKeySequence("CTRL+SHIFT+C"), this), &QShortcut::activated, [this]() {
        auto state = property("documentOnly").toBool();
        if (state) {
            ui->horizontalSplitterTop->restoreState(property("topSplitterState").toByteArray());
            ui->splitterDocumentViewer->restoreState(property("docSplitterState").toByteArray());
        } else {
            setProperty("topSplitterState", ui->horizontalSplitterTop->saveState());
            setProperty("docSplitterState", ui->splitterDocumentViewer->saveState());
            constexpr auto DEFAULT_SPLITTER_SIZE = 100;
            ui->horizontalSplitterTop->setSizes({ 0, DEFAULT_SPLITTER_SIZE });
            ui->splitterDocumentViewer->setSizes({ DEFAULT_SPLITTER_SIZE, 0 });
            ui->documentContainer->setFocus();
        }
        setProperty("documentOnly", !state);
    });

    connect(ui->buttonReload, &QToolButton::clicked, priv->projectManager, &ProjectManager::reloadProject);
    connect(priv->projectManager, &ProjectManager::projectOpened, [this](const QString& makefile) {
        for(auto& btn: ui->projectButtons->buttons()) btn->setEnabled(true);
        QFileInfo mkInfo(makefile);
        auto filepath = mkInfo.absoluteFilePath();
        auto dirpath = mkInfo.absolutePath();
        ui->stackedWidget->setCurrentWidget(ui->mainPage);
        priv->fileManager->openPath(dirpath);
        AppConfig::instance().appendToRecentProjects(filepath);
        AppConfig::instance().save();
        qputenv("CURRENT_PROJECT_FILE", filepath.toLocal8Bit());
        qputenv("CURRENT_PROJECT_DIR", dirpath.toLocal8Bit());
    });
    connect(priv->projectManager, &ProjectManager::projectClosed, [this, makeRecentProjects]() {
        qputenv("CURRENT_PROJECT_FILE", "");
        qputenv("CURRENT_PROJECT_DIR", "");
        bool ok = ui->documentContainer->aboutToCloseAll();
        qDebug() << "can close" << ok;
        if (ok) {
            for(auto& btn: ui->projectButtons->buttons()) btn->setEnabled(false);
            makeRecentProjects();
            ui->stackedWidget->setCurrentWidget(ui->welcomePage);
            priv->fileManager->closePath();
        }
    });

    auto enableEdition = [this]() {
        auto haveDocuments = ui->documentContainer->documentCount() > 0;
        auto current = ui->documentContainer->documentEditorCurrent();
        auto isModified = current? current->isModified() : false;
        ui->documentSelector->setEnabled(haveDocuments);
        ui->buttonDocumentClose->setEnabled(haveDocuments);
        ui->buttonDocumentCloseAll->setEnabled(haveDocuments);
        ui->buttonDocumentReload->setEnabled(haveDocuments);
        ui->buttonDocumentSave->setEnabled(isModified);
        ui->buttonDocumentSaveAll->setEnabled(ui->documentContainer->unsavedDocuments().count() > 0);
        if (current) {
            auto m = qobject_cast<QFileSystemModel*>(ui->fileViewer->model());
            if (m) {
                auto i = m->index(current->path());
                if (i.isValid()) {
                    ui->fileViewer->setCurrentIndex(i);
                }
            }
        }
    };
    connect(ui->documentContainer, &DocumentManager::documentModified, [this](const QString& path, IDocumentEditor *iface, bool modify){
        Q_UNUSED(path)
        Q_UNUSED(iface)
        ui->buttonDocumentSave->setEnabled(modify);
        ui->buttonDocumentSaveAll->setEnabled(ui->documentContainer->unsavedDocuments().count() > 0);
    });

    connect(ui->documentContainer, &DocumentManager::documentFocushed, enableEdition);
    connect(ui->documentContainer, &DocumentManager::documentClosed, enableEdition);

    connect(priv->projectManager, &ProjectManager::requestFileOpen, ui->documentContainer, &DocumentManager::openDocument);
    connect(ui->buttonDocumentClose, &QToolButton::clicked, ui->documentContainer, &DocumentManager::closeCurrent);
    connect(ui->buttonDocumentCloseAll, &QToolButton::clicked, ui->documentContainer, &DocumentManager::aboutToCloseAll);
    connect(ui->buttonDocumentSave, &QToolButton::clicked, ui->documentContainer, &DocumentManager::saveCurrent);
    connect(ui->buttonDocumentSaveAll, &QToolButton::clicked, ui->documentContainer, &DocumentManager::saveAll);
    connect(ui->buttonDocumentReload, &QToolButton::clicked, ui->documentContainer, &DocumentManager::reloadDocumentCurrent);

    auto setExternalTools = [this]() {
        auto m = ExternalToolManager::makeMenu(this, priv->pman, priv->projectManager);
        ui->buttonTools->setMenu(m);
        // ui->buttonExternalTools->setMenu(m);
    };
    setExternalTools();

    connect(&AppConfig::instance(), &AppConfig::configChanged, [this, setExternalTools](AppConfig *cfg) {
        if (ui->buttonTools->menu())
            ui->buttonTools->menu()->deleteLater();
        setExternalTools();
        ui->buttonDebugLaunch->setVisible(cfg->useDevelopMode());
    });

    auto findInFilesDialog = new FindInFilesDialog(this);
    auto findInFilesCallback = [this, findInFilesDialog]() {
        auto path = priv->projectManager->projectPath();
        if (!path.isEmpty()) {
            connect(findInFilesDialog, &FindInFilesDialog::queryToOpen,
                    [this](const QString& path, int line, int column)
            {
                activateWindow();
                ui->documentContainer->setFocus();
                ui->documentContainer->openDocumentHere(path, line, column);
            });
            if (findInFilesDialog->findPath().isEmpty())
                findInFilesDialog->setFindPath(path);
            findInFilesDialog->show();
        }
    };

    connect(ui->buttonFindAll, &QToolButton::clicked, findInFilesCallback);
    connect(new QShortcut(QKeySequence("CTRL+SHIFT+F"), this), &QShortcut::activated, findInFilesCallback);

    connect(ui->buttonQuit, &QToolButton::clicked, this, &MainWindow::close);
    connect(new QShortcut(QKeySequence("ALT+F4"), this), &QShortcut::activated, this, &MainWindow::close);

    connect(ui->buttonDebugLaunch, &QToolButton::toggled, [this](bool en) {
        if (en) {
            ui->bottomLeftStack->setCurrentWidget(ui->pageDebug);
        } else {
            ui->bottomLeftStack->setCurrentWidget(ui->actionViewer);
        }
    });
}

MainWindow::~MainWindow()
{
    TextMessageBrocker::instance().disconnect();
    delete priv;
    delete ui;
}

void MainWindow::openProject(const QString &path)
{
    priv->projectManager->openProject(path);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    auto unsaved = ui->documentContainer->unsavedDocuments();
    if (unsaved.isEmpty()) {
        event->accept();
    } else {
        UnsavedFilesDialog d(unsaved, this);
        event->setAccepted(d.exec() == QDialog::Accepted);
        if (event->isAccepted())
            ui->documentContainer->saveDocuments(d.checkedForSave());
    }
}
