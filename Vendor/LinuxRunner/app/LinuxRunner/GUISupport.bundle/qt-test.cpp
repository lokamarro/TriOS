// Real Linux Qt Widgets compatibility probe. GPLv3/GPLv2, as LinuxRunner.
#include <QtWidgets/QtWidgets>
#include <sys/utsname.h>
#include <cstdio>

class ProbeWindow : public QMainWindow {
    QLabel *counter, *events;
    QLineEdit *line;
    QTextEdit *text;
    unsigned clicks = 0, keys = 0, hovers = 0;
public:
    ProbeWindow() {
        setWindowTitle("LinuxRunner — real Linux Qt test");
        resize(620,460); move(10,10);
        auto *central = new QWidget(this); setCentralWidget(central);
        auto *layout = new QVBoxLayout(central); layout->setSpacing(4);
        layout->addWidget(new QLabel("REAL LINUX ARM64 QT APPLICATION"));
        struct utsname machine{}; uname(&machine);
        auto info=QString("Qt %1 | QPA %2 | DISPLAY %3 | %4 | QWidget raster / X11 software")
            .arg(qVersion(),QGuiApplication::platformName(),qEnvironmentVariable("DISPLAY"),machine.machine);
        auto *runtime=new QLabel(info); runtime->setWordWrap(true); layout->addWidget(runtime);
        counter=new QLabel("Clicks: 0"); events=new QLabel("Keys: 0 | hover: 0");
        auto *row=new QHBoxLayout; row->addWidget(counter); row->addWidget(events); layout->addLayout(row);
        line=new QLineEdit; line->setPlaceholderText("Type here: letters, punctuation, modifiers, Unicode");
        line->setObjectName("input"); layout->addWidget(line);
        connect(line,&QLineEdit::textChanged,this,[](const QString &s){
            fprintf(stdout,"QT_TEST: text_utf8_hex=%s\n",s.toUtf8().toHex().constData()); fflush(stdout);
        });
        text=new QTextEdit; text->setPlaceholderText("Multiline text: Enter, Tab/focus, arrows, copy/paste");
        text->setMaximumHeight(75); layout->addWidget(text);
        auto *buttons=new QHBoxLayout;
        auto button=[&](const char *title,auto callback){auto *b=new QPushButton(title);buttons->addWidget(b);connect(b,&QPushButton::clicked,this,callback);return b;};
        button("Count",[this]{counter->setText(QString("Clicks: %1").arg(++clicks));fprintf(stdout,"QT_TEST: clicks=%u\n",clicks);fflush(stdout);});
        button("Dialog",[this]{fprintf(stdout,"QT_TEST: modal_open\n");fflush(stdout);QMessageBox::information(this,"Real Qt modal","Linux Qt dialog. Click OK to return.");fprintf(stdout,"QT_TEST: modal_closed\n");fflush(stdout);});
        button("Child",[this]{auto *child=new QWidget(this,Qt::Window);child->setAttribute(Qt::WA_DeleteOnClose);child->setWindowTitle("Real Qt child window");auto *l=new QVBoxLayout(child);l->addWidget(new QLabel("Second Linux Qt window"));auto *edit=new QLineEdit;edit->setPlaceholderText("Child focus test");l->addWidget(edit);auto *close=new QPushButton("Close child");l->addWidget(close);connect(close,&QPushButton::clicked,child,&QWidget::close);child->resize(300,130);child->move(160,180);child->show();fprintf(stdout,"QT_TEST: child_open\n");fflush(stdout);});
        button("Copy",[this]{QApplication::clipboard()->setText(line->text());fprintf(stdout,"QT_TEST: guest_clipboard_copy\n");fflush(stdout);});
        button("Paste",[this]{line->insert(QApplication::clipboard()->text());fprintf(stdout,"QT_TEST: guest_clipboard_paste\n");fflush(stdout);});
        button("Quit",[this]{close();}); layout->addLayout(buttons);
        auto *check=new QCheckBox("Real Qt checkbox");layout->addWidget(check);
        connect(check,&QCheckBox::toggled,this,[](bool checked){fprintf(stdout,"QT_TEST: checkbox=%d\n",checked);fflush(stdout);});
        auto *slider=new QSlider(Qt::Horizontal);slider->setRange(0,100);slider->setValue(50);layout->addWidget(slider);
        connect(slider,&QSlider::valueChanged,this,[](int n){fprintf(stdout,"QT_TEST: slider=%d\n",n);fflush(stdout);});
        auto *scroll=new QScrollArea;scroll->setWidgetResizable(true);auto *items=new QWidget;auto *il=new QVBoxLayout(items);
        for(int i=1;i<=25;i++)il->addWidget(new QLabel(QString("Scrollable Linux Qt row %1").arg(i)));
        scroll->setWidget(items);layout->addWidget(scroll,1);
        auto *menu=menuBar()->addMenu("Test");
        menu->addAction("Focus text",line,qOverload<>(&QWidget::setFocus));
        menu->addAction("Resize window",this,[this]{resize(width()==620?500:620,height()==460?380:460);});
        menu->addAction("Quit",this,&QWidget::close);
        central->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(central,&QWidget::customContextMenuRequested,this,[central](const QPoint &p){QMenu popup;popup.addAction("Actual Linux Qt context menu");popup.addAction("Dismiss");popup.exec(central->mapToGlobal(p));fprintf(stdout,"QT_TEST: context_closed\n");fflush(stdout);});
        qApp->installEventFilter(this);line->setFocus();
        fprintf(stdout,"QT_TEST: startup %s\n",info.toUtf8().constData());fflush(stdout);
    }
    bool eventFilter(QObject *obj,QEvent *event) override {
        if((event->type()==QEvent::KeyPress || event->type()==QEvent::KeyRelease) && obj==QApplication::focusWidget()){
            auto *key=static_cast<QKeyEvent*>(event);
            // Count each event once at its target; propagated parent events are separate.
            if(event->type()==QEvent::KeyPress){keys++;events->setText(QString("Keys: %1 | hover: %2").arg(keys).arg(hovers));}
            fprintf(stdout,"QT_TEST: key=%08x down=%d mods=%08x text_hex=%s target=%s\n",key->key(),event->type()==QEvent::KeyPress,int(key->modifiers()),key->text().toUtf8().toHex().constData(),obj->metaObject()->className());fflush(stdout);
        }else if(event->type()==QEvent::MouseButtonPress||event->type()==QEvent::MouseButtonRelease){auto *mouse=static_cast<QMouseEvent*>(event);fprintf(stdout,"QT_TEST: pointer down=%d button=%d target=%s\n",event->type()==QEvent::MouseButtonPress,int(mouse->button()),obj->metaObject()->className());fflush(stdout);
        }else if(event->type()==QEvent::Enter){hovers++;events->setText(QString("Keys: %1 | hover: %2").arg(keys).arg(hovers));}
        else if(event->type()==QEvent::Resize && obj==this){auto *size=static_cast<QResizeEvent*>(event);fprintf(stdout,"QT_TEST: resize=%dx%d\n",size->size().width(),size->size().height());fflush(stdout);}
        return QMainWindow::eventFilter(obj,event);
    }
};
int main(int argc,char **argv){QApplication app(argc,argv);ProbeWindow window;window.show();int status=app.exec();fprintf(stdout,"QT_TEST: clean_exit=%d\n",status);fflush(stdout);return status;}
