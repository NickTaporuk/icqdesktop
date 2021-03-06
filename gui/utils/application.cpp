#include "stdafx.h"
#include "application.h"

#include "InterConnector.h"
#include "launch.h"
#include "utils.h"
#include "log/log.h"
#include "animation/animation.h"
#include "../core_dispatcher.h"
#include "../gui_settings.h"
#include "../cache/emoji/Emoji.h"
#include "../cache/stickers/stickers.h"
#include "../themes/Themes.h"
#include "../main_window/MainWindow.h"
#include "../main_window/contact_list/ContactListModel.h"
#include "../main_window/contact_list/RecentsModel.h"
#include "../main_window/contact_list/UnknownsModel.h"
#include "../../common.shared/version_info.h"

#include "../main_window/mplayer/FFMpegPlayer.h"
#include "../main_window/sounds/SoundsManager.h"

#ifdef __linux__
#include <sys/file.h>
#endif //__linux__


namespace
{
    const int SHADOW_WIDTH = 10;
}

namespace Utils
{
#ifdef _WIN32
    AppGuard::AppGuard()
        : Mutex_(0)
        , Exist_(false)
    {
        Mutex_ = ::CreateSemaphore(NULL, 0, 1, Utils::get_crossprocess_mutex_name());
        Exist_ = GetLastError() == ERROR_ALREADY_EXISTS;
    }

    AppGuard::~AppGuard()
    {
        if (Mutex_)
            CloseHandle(Mutex_);
    }

    bool AppGuard::succeeded() const
    {
        return !Exist_;
    }

    /**
     * To fix crash in voip on shutting down PC,
     * We process WM_QUERYENDSESSION/WM_ENDSESSION and Windows will wait our deinitialization.
     */
    class WindowsEventFilter : public QAbstractNativeEventFilter
    {
    public:
        WindowsEventFilter() {}

        virtual bool nativeEventFilter(const QByteArray& eventType, void* message, long* result) override
        {
            MSG* msg = reinterpret_cast<MSG*>(message);
            if (msg->message == WM_QUERYENDSESSION || msg->message == WM_ENDSESSION)
            {
                if (!Ui::GetDispatcher()->getVoipController().isVoipKilled())
                {
                    Ui::GetDispatcher()->getVoipController().voipReset();

                    // Under XP we can stop shutting down PC, if we return true.
                    if (QSysInfo().windowsVersion() != QSysInfo::WV_XP)
                    {
                        // Do not shutting down us immediately, we need time to terminate
                        *result = 0;
                        return true;
                    }
                }
            }

            return false;
        }
    };
#endif //_WIN32

    Application::Application(int& _argc, char* _argv[])
        : QObject(0)
    {
#ifndef _WIN32
    #ifdef __APPLE__
        #ifndef ICQ_QT_STATIC
            QDir dir(_argv[0]);
            dir.cdUp();
            dir.cdUp();
            dir.cd(qsl("PlugIns"));
            QCoreApplication::setLibraryPaths(QStringList(dir.absolutePath()));
        #endif
    #else // linux
        std::string appDir(_argv[0]);
        appDir = appDir.substr(0, appDir.rfind("/") + 1);
        appDir += "plugins";
        QCoreApplication::setLibraryPaths(QStringList(QString::fromStdString(appDir)));
    #endif //__APPLE__
#endif //_WIN32

        app_ = std::make_unique<QApplication>(_argc, _argv);
        app_->setProperty("startupTime", QDateTime::currentMSecsSinceEpoch());

#ifdef __APPLE__
        // Fix float separator: force use . (dot).
        // http://doc.qt.io/qt-5/qcoreapplication.html#locale-settings
        setlocale(LC_NUMERIC, "C");
#endif
        makeBuild();

#ifdef _WIN32
        guard_ = std::make_unique<Utils::AppGuard>();
#endif //_WIN32


        peer_ = std::make_unique<LocalPeer>(nullptr, !isMainInstance());
        if (isMainInstance())
        {
            peer_->listen();
        }

        QObject::connect(&Utils::InterConnector::instance(), &Utils::InterConnector::schemeUrlClicked, this, &Application::receiveUrlCommand, Qt::DirectConnection);
        QObject::connect(app_.get(), &QGuiApplication::applicationStateChanged, this, &Application::applicationStateChanged, Qt::DirectConnection);
    }

    Application::~Application()
    {
        mainWindow_.reset();
        anim::AnimationManager::stopManager();

        Emoji::Cleanup();
#ifndef INTERNAL_RESOURCES
        QResource::unregisterResource(QApplication::applicationDirPath() + ql1s("/qresource"));
#endif

        Ui::ResetMediaContainer();

        Logic::ResetRecentsModel();
        Logic::ResetUnknownsModel();
        Logic::ResetContactListModel();

        Ui::ResetSoundsManager();
    }

    void Application::makeBuild()
    {
        const auto path = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
        if (path.contains(ql1s("agent"), Qt::CaseInsensitive))
        {
            build::is_build_icq = false;
            return;
        }

        const auto params = QCoreApplication::arguments();
        build::is_build_icq = std::none_of(params.begin(), params.end(), [](const auto& param) { return param == ql1s("/agent"); });
    }

    int Application::exec()
    {
#ifdef _WIN32
        WindowsEventFilter eventFilter;
        qApp->installNativeEventFilter(&eventFilter);
#endif

        int res = app_->exec();
#ifdef _WIN32
        qApp->removeNativeEventFilter(&eventFilter);
#endif

        Ui::destroyDispatcher();

        return res;
    }

    bool Application::init()
    {
        Ui::createDispatcher();

        init_win7_features();

        app_->setAttribute(Qt::AA_SynthesizeMouseForUnhandledTouchEvents, false);
#ifndef INTERNAL_RESOURCES
        QResource::registerResource(app_->applicationDirPath() + ql1s("/qresource"));
#endif

        QPixmapCache::setCacheLimit(0);

        QObject::connect(Ui::GetDispatcher(), &Ui::core_dispatcher::coreLogins, this, &Application::coreLogins, Qt::DirectConnection);
        QObject::connect(Ui::GetDispatcher(), &Ui::core_dispatcher::guiSettings, this, &Application::guiSettings, Qt::DirectConnection);

        return true;
    }

    void Application::init_win7_features()
    {
        Utils::groupTaskbarIcon(true);
    }

    bool Application::isMainInstance()
    {
#if defined _WIN32
        return guard_->succeeded();
#elif defined __linux__
        int fd;
        struct flock fl;
        fd = open(qsl("/tmp/%1.pid").arg(build::is_icq() ? qsl("icq") : qsl("magent")).toStdString().c_str(), O_CREAT | O_RDWR, 0666);
        if(fd == -1)
        {
            return true;
        }

        fl.l_type   = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start  = 0;
        fl.l_len    = 0;
        fl.l_pid    = QCoreApplication::applicationPid();

        if( fcntl(fd, F_SETLK, &fl) == -1)
        {
            if( errno == EACCES || errno == EAGAIN)
            {
                 return false;
            }
        }
        return true;
#else
        return true;
#endif //_WIN32
    }

    void Application::switchInstance(launch::CommandLineParser& _cmdParser)
    {
#ifdef _WIN32

        if (_cmdParser.isUrlCommand())
        {
            peer_->send_url_command(_cmdParser.getUrlCommand());
        }
#endif //_WIN32
        unsigned int hwnd = peer_->get_hwnd_and_activate();

        if (hwnd)
        {
#ifdef _WIN32
            ::SetForegroundWindow((HWND) hwnd);
#endif //_WIN32
        }
    }

    void Application::coreLogins(const bool _has_valid_login)
    {
        initMainWindow(_has_valid_login);
    }

    void Application::guiSettings()
    {
        auto dir = Ui::get_gui_settings()->get_value(settings_download_directory, QString());
        if (!dir.length())
            Ui::get_gui_settings()->set_value(settings_download_directory, Utils::DefaultDownloadsPath()); // workaround for core
    }

    void Application::initMainWindow(const bool _has_valid_login)
    {
        anim::AnimationManager::startManager();

        double dpi = app_->primaryScreen()->logicalDotsPerInchX();

        if (platform::is_apple())
        {
            dpi = 96;
            Utils::set_mac_retina(app_->primaryScreen()->devicePixelRatio() == 2);
            if (Utils::is_mac_retina())
                app_->setAttribute(Qt::AA_UseHighDpiPixmaps);
        }

        QFontDatabase::addApplicationFont(qsl(":/fonts/openSans_bold"));
        QFontDatabase::addApplicationFont(qsl(":/fonts/openSans_light"));
        QFontDatabase::addApplicationFont(qsl(":/fonts/openSans_medium"));
        QFontDatabase::addApplicationFont(qsl(":/fonts/openSans_regular"));

        const auto guiScaleCoefficient = std::min(dpi / 96.0, 2.0);

        Utils::initBasicScaleCoefficient(guiScaleCoefficient);

        Utils::setScaleCoefficient(Ui::get_gui_settings()->get_value<double>(settings_scale_coefficient, Utils::getBasicScaleCoefficient()));
        app_->setStyleSheet(Utils::LoadStyle(qsl(":/qss/common_style")));
        app_->setFont(Fonts::appFontFamilyNameQss(Fonts::defaultAppFontFamily(), Fonts::FontWeight::Normal));

        Themes::SetCurrentThemeId(Themes::ThemeId::Default);
        Emoji::InitializeSubsystem();

        Ui::get_gui_settings()->set_shadow_width(Utils::scale_value(SHADOW_WIDTH));

        Utils::GetTranslator()->init();
        mainWindow_ = std::make_unique<Ui::MainWindow>(app_.get(), _has_valid_login);

        bool needToShow = true;
#ifdef _WIN32
        const auto arguments = app_->arguments();
        for (const auto& argument : arguments)
        {
            if (argument == ql1s("/startup") && Ui::get_gui_settings()->get_value<bool>(settings_start_minimazed, false))
            {
                needToShow = false;
                mainWindow_->hide();
                break;
            }
        }
#endif //_WIN32
        if (needToShow)
            mainWindow_->show();

        mainWindow_->activateWindow();
        peer_->set_main_window(mainWindow_.get());
    }

    void Application::parseUrlCommand(const QString& _urlCommand)
    {
        const QUrl url(_urlCommand);

        const QString host = url.host();

        const QUrlQuery urlQuery(url);

        const auto items = urlQuery.queryItems();

        const QString path = url.path();

        if (host == ql1s(url_command_join_livechat))
        {
            if (path.isEmpty())
                return;

            QString stamp = path.mid(1);
            if (stamp.isEmpty())
                return;

            if (stamp.endsWith(ql1c('/')))
                stamp.chop(1);

            const bool silent = std::any_of(items.cbegin(), items.cend(), [](const auto& param) {
                return param.first == ql1s("join") && param.second == ql1s("1");
            });

            if (mainWindow_)
            {
                mainWindow_->activate();
                mainWindow_->raise();
            }

            Logic::getContactListModel()->joinLiveChat(stamp, silent);

        }
        else if (host == ql1s(url_command_open_profile))
        {
            const QString aimId = path.mid(1);
            if (aimId.isEmpty())
                return;

            if (mainWindow_)
            {
                mainWindow_->activate();
                mainWindow_->raise();
            }


            emit Utils::InterConnector::instance().profileSettingsShow(aimId);
        }
        else if (host == ql1s(url_command_app))
        {
            const QStringRef command = path.midRef(1);
            if (command == ql1s(url_command_livechats_home))
                emit Utils::InterConnector::instance().liveChatsShow();
        }
        else if (host == ql1s(url_command_stickerpack_info))
        {
            if (path.isEmpty())
                return;

            QString storeId = path.mid(1);
            if (storeId.isEmpty())
                return;

            if (storeId.endsWith(ql1c('/')))
                storeId.chop(1);

            Ui::Stickers::showStickersPack(storeId);
        }
    }

    void Application::receiveUrlCommand(const QString& _urlCommand)
    {
        parseUrlCommand(_urlCommand);
    }

    bool Application::updating()
    {
#ifdef _WIN32

        const auto updater_singlton_mutex_name = (build::is_icq() ? updater_singlton_mutex_name_icq : updater_singlton_mutex_name_agent);

        CHandle mutex(::CreateSemaphore(NULL, 0, 1, updater_singlton_mutex_name.c_str()));
        if (ERROR_ALREADY_EXISTS == ::GetLastError())
            return true;

        CRegKey keySoftware;

        if (ERROR_SUCCESS != keySoftware.Open(HKEY_CURRENT_USER, L"Software"))
            return false;

        CRegKey key_product;
        auto productName = getProductName();
        if (ERROR_SUCCESS != key_product.Create(keySoftware, (const wchar_t*) productName.utf16()))
            return false;

        wchar_t versionBuffer[1025];
        unsigned long len = 1024;
        if (ERROR_SUCCESS != key_product.QueryStringValue(L"update_version", versionBuffer, &len))
            return false;

        QString versionUpdate = QString::fromUtf16((const ushort*)versionBuffer);

        core::tools::version_info infoCurrent;
        core::tools::version_info infoUpdate(versionUpdate.toStdString());

        if (infoCurrent < infoUpdate)
        {
            wchar_t thisModuleName[1024];
            if (!::GetModuleFileName(0, thisModuleName, 1024))
                return false;

            QDir dir = QFileInfo(QString::fromUtf16((const ushort*)thisModuleName)).absoluteDir();
            if (!dir.cdUp())
                return false;

            const QString updateFolder = dir.absolutePath() % ql1c('/') % updates_folder_short  % ql1c('/') % versionUpdate;

            QDir updateDir(updateFolder);
            if (updateDir.exists())
            {
                const QString setupName = updateFolder % ql1c('/') % Utils::getInstallerName();
                QFileInfo setupInfo(setupName);
                if (!setupInfo.exists())
                    return false;

                mutex.Close();

                const QString command = ql1c('"') % QDir::toNativeSeparators(setupName) % ql1s("\" ") % update_final_command;
                QProcess::startDetached(command);

                return true;
            }
        }
#endif //_WIN32

        return false;
    }

    void Application::applicationStateChanged(Qt::ApplicationState state)
    {
        if (platform::is_apple() && state == Qt::ApplicationInactive)
        {
            emit Utils::InterConnector::instance().closeAnyPopupMenu();
            emit Utils::InterConnector::instance().closeAnyPopupWindow();
        }
    }
}
