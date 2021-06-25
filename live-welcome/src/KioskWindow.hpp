/*
 */

#pragma once

#include <QtCore/QProcess>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabBar>

#include "KioskTabs.hpp"
#include "KioskSettingsPopup.hpp"
#include "Utils.hpp"

#include "../widgets/digitalpeakmeter.hpp"

#define SERVER_MODE
#include "sys_host/sys_host_impl.h"

class PeakMeterThread : public QThread
{
    AudioContainerComm* containerComm;
    DigitalPeakMeter& peakMeterIn;
    DigitalPeakMeter& peakMeterOut;

    int sys_host_shmfd;
    sys_serial_shm_data* sys_host_data;

public:
    explicit PeakMeterThread(QObject* const parent, DigitalPeakMeter& in, DigitalPeakMeter& out)
      : QThread(parent),
        containerComm(nullptr),
        peakMeterIn(in),
        peakMeterOut(out),
        sys_host_shmfd(-1),
        sys_host_data(nullptr) {}

    void init()
    {
        if ((containerComm = initAudioContainerComm()) != nullptr)
        {
            if (sys_serial_open(&sys_host_shmfd, &sys_host_data))
                fprintf(stdout, "sys_host shared memory ok!\n");
            else
                fprintf(stderr, "sys_host shared memory failed\n");

            start(HighPriority);
        }
    }

    void stop()
    {
        cleanupAudioContainerComm(containerComm);
        sys_serial_close(sys_host_shmfd, sys_host_data);
        wait(2000);
    }

    void send(const sys_serial_event_type etype, const int value)
    {
        if (sys_host_data == nullptr)
            return;

        char str[24];
        snprintf(str, sizeof(str), "%i", value);
        str[sizeof(str)-1] = '\0';

        if (! sys_serial_write(&sys_host_data->client, etype, str))
            return;

        sem_post(&sys_host_data->client.sem);
    }

    void run() override
    {
        float peaks[4];
        char msg[SYS_SERIAL_SHM_DATA_SIZE];
        sys_serial_event_type etype;
        uint8_t page, subpage;

        sys_serial_shm_data_channel* const sysdata = sys_host_data != nullptr ? &sys_host_data->server : nullptr;

        while (containerComm != nullptr && ! isInterruptionRequested())
        {
            // flush all incoming host events
            if (sys_host_data != nullptr && sem_trywait(&sysdata->sem) == 0)
            {
                while (sysdata->head != sysdata->tail)
                    sys_serial_read(sysdata, &etype, &page, &subpage, msg);
            }

            if (! waitForAudioContainerComm(containerComm))
                continue;

            memcpy(peaks, containerComm->peaks, sizeof(peaks));
            peakMeterIn.displayMeter(1, peaks[0]);
            peakMeterIn.displayMeter(2, peaks[1]);
            peakMeterOut.displayMeter(1, peaks[2]);
            peakMeterOut.displayMeter(2, peaks[3]);
        }
    }
};

class KioskWindow : public QMainWindow
{
    Q_OBJECT

    KioskTabs tabWidget;
    KioskSettingsPopup* settingsPopup;

    QFont clockFont;
    QRect clockRect;
    int clockTimer;

    QSlider gainSlider;
    DigitalPeakMeter peakMeterIn;
    DigitalPeakMeter peakMeterOut;
    QPushButton settingsButton;
    QPushButton powerButton;

    const QString program;
    QProcess audioContainer;

    PeakMeterThread peakMeterThread;

public:
    KioskWindow()
      : QMainWindow(),
        settingsPopup(nullptr),
        tabWidget(this),
        clockFont(font()),
        clockRect(),
        clockTimer(-1),
        gainSlider(Qt::Horizontal, this),
        peakMeterIn(this),
        peakMeterOut(this),
        settingsButton(this),
        powerButton(this),
        program(findStartScript()),
        audioContainer(),
        peakMeterThread(this, peakMeterIn, peakMeterOut)
    {
        setCentralWidget(&tabWidget);
        setWindowTitle("MOD Live USB");

        audioContainer.setProcessChannelMode(QProcess::ForwardedChannels);

        const int height = tabWidget.tabBar()->height();

        clockFont.setFamily("Monospace");
        clockFont.setPixelSize(20);

        gainSlider.setFixedSize(height*4, height);
        gainSlider.setFocusPolicy(Qt::FocusPolicy::NoFocus);
        gainSlider.setMinimum(-30);
        gainSlider.setMaximum(30);
        gainSlider.setTickPosition(QSlider::TicksBothSides);

        peakMeterIn.setChannelCount(2);
        peakMeterIn.setMeterColor(DigitalPeakMeter::COLOR_BLUE);
        peakMeterIn.setMeterLinesEnabled(false);
        peakMeterIn.setMeterOrientation(DigitalPeakMeter::HORIZONTAL);
        peakMeterIn.setMeterStyle(DigitalPeakMeter::STYLE_RNCBC);
        peakMeterIn.setSmoothMultiplier(0);
        peakMeterIn.setFixedSize(150, height);

        peakMeterOut.setChannelCount(2);
        peakMeterOut.setMeterColor(DigitalPeakMeter::COLOR_GREEN);
        peakMeterOut.setMeterLinesEnabled(false);
        peakMeterOut.setMeterOrientation(DigitalPeakMeter::HORIZONTAL);
        peakMeterOut.setMeterStyle(DigitalPeakMeter::STYLE_OPENAV);
        peakMeterOut.setSmoothMultiplier(0);
        peakMeterOut.setFixedSize(150, height);

        settingsButton.setFixedSize(height, height);
        settingsButton.setFocusPolicy(Qt::FocusPolicy::NoFocus);
        settingsButton.setText("(S)");

        powerButton.setFixedSize(height, height);
        powerButton.setFocusPolicy(Qt::FocusPolicy::NoFocus);
        powerButton.setText("(P)");

        repositionTabBarWidgets();

        clockTimer = startTimer(1000);

        connect(&powerButton, SIGNAL(clicked()), this, SLOT(openPower()));
        connect(&settingsButton, SIGNAL(clicked()), this, SLOT(openSettings()));
        connect(&gainSlider, SIGNAL(valueChanged(int)), this, SLOT(setGain(int)));

        if (program.isEmpty())
            settingsButton.hide();
        else
            peakMeterThread.init();

        if (getenv("TESTING") != nullptr)
            tryConnectingToWebServer();
    }

    ~KioskWindow() override
    {
        peakMeterThread.requestInterruption();
        stopAudioContainer();
        peakMeterThread.stop();
    }

    void stopAudioContainer()
    {
        if (getenv("USING_SYSTEMD") != nullptr)
        {
            QProcess sctl;
            sctl.setProcessChannelMode(QProcess::ForwardedChannels);
            sctl.start("systemctl", {"stop", "mod-live-audio"}, QIODevice::ReadWrite);
            sctl.waitForFinished();
        }

        if (getenv("USING_SYSTEMD") != nullptr || getuid() == 0)
        {
            QProcess mctl;
            mctl.setProcessChannelMode(QProcess::ForwardedChannels);
            mctl.start("machinectl", {"stop", "mod-live-usb"}, QIODevice::ReadWrite);
            mctl.waitForFinished();
        }

        if (audioContainer.state() != QProcess::NotRunning)
        {
            audioContainer.terminate();
            audioContainer.waitForFinished();
        }
    }

public Q_SLOTS:
    void openPower()
    {
        if (QMessageBox::question(this,
                                  "Power Off",
                                  "Power off the system now?",
                                  QMessageBox::StandardButtons(QMessageBox::Yes|QMessageBox::No)) == QMessageBox::Yes)
        {
            system("poweroff");
        }
    }

    void openSettings(const bool cancellable = true)
    {
        if (program.isEmpty())
            return;

        if (settingsPopup == nullptr)
            settingsPopup = new KioskSettingsPopup();

        settingsPopup->setCancellable(cancellable);

        if (settingsPopup->exec() == 0 && cancellable)
            return;

        QString device;
        unsigned rate;
        unsigned bufsize;

        if (! settingsPopup->getSelected(device, rate, bufsize))
        {
            if (cancellable)
                return;

            return openSettings(cancellable);
        }
        printf("Selected %s %u %u\n", device.toUtf8().constData(), rate, bufsize);

        stopAudioContainer();

        const QStringList arguments = { device, QString::number(rate), QString::number(bufsize) };

        audioContainer.start(program, arguments, QIODevice::ReadWrite | QIODevice::Unbuffered);

        if (getenv("USING_SYSTEMD") != nullptr)
            audioContainer.waitForFinished();

        QTimer::singleShot(0, this, SLOT(tryConnectingToWebServer()));
    }

    void tryConnectingToWebServer()
    {
        QTcpSocket socket;
        socket.connectToHost("localhost", 8888);

        if (! socket.waitForConnected(500))
        {
            QTimer::singleShot(500, this, SLOT(tryConnectingToWebServer()));
            return;
        }

        tabWidget.reloadPage();
    }

    void setGain(const int gain)
    {
        peakMeterThread.send(sys_serial_event_type_pedalboard_gain, gain);
    }

protected:
    void keyPressEvent(QKeyEvent* const event) override
    {
        QMainWindow::keyPressEvent(event);

        const Qt::KeyboardModifiers modifiers = event->modifiers();

        if ((modifiers & Qt::Modifier::ALT) == 0x0)
            return;
        if ((modifiers & Qt::Modifier::CTRL) == 0x0)
            return;

        if (event->key() == Qt::Key::Key_R)
            tabWidget.reloadPage();
        if (event->key() == Qt::Key::Key_T)
            tabWidget.openTerminal();
    }

    void paintEvent(QPaintEvent* const event) override
    {
        QMainWindow::paintEvent(event);

        if (clockRect.x() == 0)
            return;

        QPainter painter(this);
        painter.setFont(clockFont);
        painter.drawText(clockRect, QTime::currentTime().toString("hh:mm:ss"));
    }

    void resizeEvent(QResizeEvent* const event) override
    {
        QMainWindow::resizeEvent(event);
        repositionTabBarWidgets();
    }

    void timerEvent(QTimerEvent* const event) override
    {
        QMainWindow::timerEvent(event);

        if (event->timerId() != clockTimer)
            return;
        if (clockRect.x() == 0)
            return;

        update(clockRect);
    }

private:
    void repositionTabBarWidgets()
    {
        const int width = this->width();
        const int height = tabWidget.tabBar()->height();

        // clock at center
        QFontMetrics clockmetrics(clockFont);
        const int clockwidth = clockmetrics.horizontalAdvance("00:00:00");
        const int clockheight = clockmetrics.height();
        clockRect = QRect(width/2 - clockwidth/2, height/2 - clockheight/2, clockwidth, clockheight);

        // going from the corner..
        const int padding = 4;
        int x = width;

        x -= powerButton.width() + padding;
        powerButton.move(x, 0);

        x -= settingsButton.width() + padding;
        settingsButton.move(x, 0);

        x -= peakMeterOut.width() + padding;
        peakMeterOut.move(x, 0);

        x -= peakMeterIn.width() + padding;
        peakMeterIn.move(x, 0);

        x -= gainSlider.width() + padding;
        gainSlider.move(x, 0);
    }
};