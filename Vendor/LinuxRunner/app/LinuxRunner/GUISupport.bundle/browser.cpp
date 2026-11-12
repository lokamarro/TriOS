// Real guest Linux Qt Widgets + Chromium, compiled by guest g++.
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QStatusBar>
#include <QtGui/QAction>
#include <QtGui/QShortcut>
#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtWebEngineWidgets/QWebEngineView>
#include <QtWebEngineCore/QWebEngineProfile>
#include <QtWebEngineCore/QWebEnginePage>
#include <QtWebEngineCore/QWebEngineDownloadRequest>
#include <QtWebEngineCore/qtwebenginecoreglobal.h>
#include <sys/utsname.h>
#include <cstdio>
int main(int argc,char **argv) {
    setbuf(stdout,nullptr);setbuf(stderr,nullptr);
    utsname u{};uname(&u);
    printf("BROWSER: executable ARM64 Qt=%s Chromium=%s security=%s arch=%s\n",qVersion(),qWebEngineChromiumVersion(),qWebEngineChromiumSecurityPatchVersion(),u.machine);
    if (argc>1 && !strcmp(argv[1],"--versions")) return 0;
    QApplication app(argc,argv);
    printf("BROWSER: Qt application initialized QPA=%s DISPLAY=%s\n",qPrintable(QGuiApplication::platformName()),qgetenv("DISPLAY").constData());
    QMainWindow window;window.setWindowTitle("LinuxRunner Browser — real Linux Chromium");window.resize(620,460);window.move(10,10);
    auto *bar=window.addToolBar("Navigation");
    auto *tabs=new QTabWidget;tabs->setTabsClosable(true);tabs->setMovable(true);window.setCentralWidget(tabs);
    auto *url=new QLineEdit;url->setPlaceholderText("URL or search");url->setMinimumWidth(160);
    auto *back=bar->addAction("Back"),*forward=bar->addAction("Forward"),*reload=bar->addAction("Reload"),*stop=bar->addAction("Stop");bar->addWidget(url);auto *newTab=bar->addAction("+");
    auto *progress=new QProgressBar;progress->setMaximumWidth(70);bar->addWidget(progress);
    auto *profile=new QWebEngineProfile("LinuxRunner",&app);
    const QString base=QDir::homePath()+"/.local/share/linuxrunner-browser";QDir().mkpath(base);
    profile->setPersistentStoragePath(base+"/storage");profile->setCachePath(base+"/cache");profile->setPersistentCookiesPolicy(QWebEngineProfile::AllowPersistentCookies);
    QObject::connect(profile,&QWebEngineProfile::downloadRequested,&app,[](QWebEngineDownloadRequest *d){QString dir=QDir::homePath()+"/Downloads";QDir().mkpath(dir);d->setDownloadDirectory(dir);printf("BROWSER: download requested %s\n",qPrintable(d->downloadFileName()));QObject::connect(d,&QWebEngineDownloadRequest::isFinishedChanged,d,[d](){if(d->isFinished())printf("BROWSER: download finished state=%d bytes=%lld path=%s/%s\n",int(d->state()),(long long)d->receivedBytes(),qPrintable(d->downloadDirectory()),qPrintable(d->downloadFileName()));});d->accept();});
    auto current=[tabs](){return qobject_cast<QWebEngineView*>(tabs->currentWidget());};
    auto add=[&](QUrl location){
        auto *view=new QWebEngineView;view->setPage(new QWebEnginePage(profile,view));int n=tabs->addTab(view,"Loading…");tabs->setCurrentIndex(n);
        QObject::connect(view,&QWebEngineView::titleChanged,&window,[=,&window](const QString &title){tabs->setTabText(tabs->indexOf(view),title.left(35));window.setWindowTitle(title+" — LinuxRunner Browser");});
        QObject::connect(view,&QWebEngineView::urlChanged,&window,[=](const QUrl &v){if(tabs->currentWidget()==view)url->setText(v.toDisplayString());});
        QObject::connect(view,&QWebEngineView::loadProgress,&window,[=](int n){if(tabs->currentWidget()==view)progress->setValue(n);});
        QObject::connect(view,&QWebEngineView::loadFinished,&window,[=,&window](bool ok){printf("BROWSER: loadFinished ok=%d url=%s\n",ok,qPrintable(view->url().toString()));window.statusBar()->showMessage(ok?"Loaded":"Page failed to load");if(ok)view->page()->runJavaScript("JSON.stringify({title:document.title,text:document.body.innerText,js:1+2})",[](const QVariant &v){printf("BROWSER: JavaScript result=%s\n",qPrintable(v.toString()));});});
        QObject::connect(view->page(),&QWebEnginePage::renderProcessTerminated,&window,[](auto status,int code){printf("BROWSER: renderer terminated status=%d code=%d\n",int(status),code);});
        view->load(location);printf("BROWSER: tab load requested %s\n",qPrintable(location.toString()));
    };
    QObject::connect(back,&QAction::triggered,&window,[&](){if(current())current()->back();});QObject::connect(forward,&QAction::triggered,&window,[&](){if(current())current()->forward();});QObject::connect(reload,&QAction::triggered,&window,[&](){if(current())current()->reload();});QObject::connect(stop,&QAction::triggered,&window,[&](){if(current())current()->stop();});
    QObject::connect(url,&QLineEdit::returnPressed,&window,[&](){if(current())current()->load(QUrl::fromUserInput(url->text()));});
    QObject::connect(newTab,&QAction::triggered,&window,[&](){add(QUrl("about:blank"));});
    QObject::connect(tabs,&QTabWidget::tabCloseRequested,&window,[&](int i){auto *w=tabs->widget(i);tabs->removeTab(i);delete w;if(!tabs->count())window.close();});
    QObject::connect(tabs,&QTabWidget::currentChanged,&window,[&](int){if(current())url->setText(current()->url().toDisplayString());});
    auto shortcut=[&](const QKeySequence &keys,auto action){auto *s=new QShortcut(keys,&window);QObject::connect(s,&QShortcut::activated,&window,action);};
    shortcut(QKeySequence("Ctrl+L"),[&](){url->setFocus();url->selectAll();});
    shortcut(QKeySequence("Ctrl+T"),[&](){add(QUrl("about:blank"));});
    shortcut(QKeySequence("Ctrl+W"),[&](){int i=tabs->currentIndex();if(i>=0){auto*w=tabs->widget(i);tabs->removeTab(i);delete w;}if(!tabs->count())window.close();});
    shortcut(QKeySequence("Ctrl+Q"),[&](){window.close();});
    shortcut(QKeySequence("Ctrl++"),[&](){if(current())current()->setZoomFactor(qMin(3.0,current()->zoomFactor()+0.25));});
    shortcut(QKeySequence("Ctrl+-"),[&](){if(current())current()->setZoomFactor(qMax(0.5,current()->zoomFactor()-0.25));});
    shortcut(QKeySequence("Ctrl+0"),[&](){if(current())current()->setZoomFactor(1.0);});
    window.show();printf("BROWSER: native Linux Qt window shown; page rendering remains unverified\n");
    add(argc>1?QUrl::fromUserInput(QString::fromLocal8Bit(argv[1])):QUrl::fromLocalFile(QDir::currentPath()+"/browser-test.html"));
    int result=app.exec();printf("BROWSER: exit=%d\n",result);return result;
}
