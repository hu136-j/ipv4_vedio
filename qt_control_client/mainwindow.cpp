#include "mainwindow.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpSocket>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      socket_(new QTcpSocket(this)),
      ipEdit_(nullptr),
      portSpinBox_(nullptr),
      connectButton_(nullptr),
      disconnectButton_(nullptr),
      refreshButton_(nullptr),
      playButton_(nullptr),
      stopButton_(nullptr),
      programList_(nullptr),
      statusText_(nullptr)
{
    setupUi();
    setupConnections();
    updateUiState();
    appendStatus("GUI ready.");
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (socket_->state() == QAbstractSocket::ConnectedState)
    {
        sendCommand("QUIT");
        socket_->flush();
        socket_->disconnectFromHost();
        if (socket_->state() != QAbstractSocket::UnconnectedState)
            socket_->waitForDisconnected(300);
    }

    QMainWindow::closeEvent(event);
}

void MainWindow::connectToCore()
{
    if (socket_->state() != QAbstractSocket::UnconnectedState)
        return;

    recvBuffer_.clear();
    programList_->clear();

    appendStatus(QString("Connecting to %1:%2 ...")
                     .arg(ipEdit_->text())
                     .arg(portSpinBox_->value()));
    socket_->connectToHost(ipEdit_->text(), static_cast<quint16>(portSpinBox_->value()));
    updateUiState();
}

void MainWindow::disconnectFromCore()
{
    if (socket_->state() == QAbstractSocket::UnconnectedState)
        return;

    sendCommand("QUIT");
    socket_->disconnectFromHost();
    appendStatus("Disconnect requested.");
    updateUiState();
}

void MainWindow::refreshProgramList()
{
    programList_->clear();
    sendCommand("LIST");
}

void MainWindow::playSelectedProgram()
{
    int id = selectedProgramId();

    if (id < 0)
    {
        QMessageBox::warning(this, "No Selection", "Please select a program first.");
        return;
    }

    sendCommand(QString("PLAY %1").arg(id));
}

void MainWindow::stopPlayback()
{
    sendCommand("STOP");
}

void MainWindow::onConnected()
{
    appendStatus("TCP connected.");
    updateUiState();
}

void MainWindow::onDisconnected()
{
    appendStatus("TCP disconnected.");
    updateUiState();
}

void MainWindow::onReadyRead()
{
    recvBuffer_.append(socket_->readAll());

    while (true)
    {
        int newlinePos = recvBuffer_.indexOf('\n');
        QByteArray lineBytes;
        QString line;

        if (newlinePos < 0)
            break;

        lineBytes = recvBuffer_.left(newlinePos);
        recvBuffer_.remove(0, newlinePos + 1);

        line = QString::fromUtf8(lineBytes).trimmed();
        if (!line.isEmpty())
            processLine(line);
    }
}

void MainWindow::onSocketError()
{
    appendStatus(QString("Socket error: %1").arg(socket_->errorString()));
    updateUiState();
}

void MainWindow::setupUi()
{
    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    QGroupBox *connectionGroup = new QGroupBox("TCP Connection", centralWidget);
    QFormLayout *connectionLayout = new QFormLayout(connectionGroup);
    QWidget *buttonRow = new QWidget(connectionGroup);
    QHBoxLayout *buttonLayout = new QHBoxLayout(buttonRow);
    QGroupBox *programGroup = new QGroupBox("Program List", centralWidget);
    QVBoxLayout *programLayout = new QVBoxLayout(programGroup);
    QWidget *controlRow = new QWidget(programGroup);
    QHBoxLayout *controlLayout = new QHBoxLayout(controlRow);
    QLabel *statusLabel = new QLabel("Status", centralWidget);

    ipEdit_ = new QLineEdit("127.0.0.1", connectionGroup);
    portSpinBox_ = new QSpinBox(connectionGroup);
    connectButton_ = new QPushButton("Connect", buttonRow);
    disconnectButton_ = new QPushButton("Disconnect", buttonRow);
    refreshButton_ = new QPushButton("Refresh List", controlRow);
    playButton_ = new QPushButton("Play Selected", controlRow);
    stopButton_ = new QPushButton("Stop", controlRow);
    programList_ = new QListWidget(programGroup);
    statusText_ = new QTextEdit(centralWidget);

    setWindowTitle("Streaming Broadcast Controller");
    resize(760, 560);

    portSpinBox_->setRange(1, 65535);
    portSpinBox_->setValue(9000);

    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->addWidget(connectButton_);
    buttonLayout->addWidget(disconnectButton_);
    buttonLayout->addStretch();

    connectionLayout->addRow("Core IP:", ipEdit_);
    connectionLayout->addRow("Core Port:", portSpinBox_);
    connectionLayout->addRow(buttonRow);

    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->addWidget(refreshButton_);
    controlLayout->addWidget(playButton_);
    controlLayout->addWidget(stopButton_);
    controlLayout->addStretch();

    programLayout->addWidget(programList_);
    programLayout->addWidget(controlRow);

    statusText_->setReadOnly(true);

    mainLayout->addWidget(connectionGroup);
    mainLayout->addWidget(programGroup);
    mainLayout->addWidget(statusLabel);
    mainLayout->addWidget(statusText_);

    setCentralWidget(centralWidget);
}

void MainWindow::setupConnections()
{
    connect(connectButton_, &QPushButton::clicked,
            this, &MainWindow::connectToCore);
    connect(disconnectButton_, &QPushButton::clicked,
            this, &MainWindow::disconnectFromCore);
    connect(refreshButton_, &QPushButton::clicked,
            this, &MainWindow::refreshProgramList);
    connect(playButton_, &QPushButton::clicked,
            this, &MainWindow::playSelectedProgram);
    connect(stopButton_, &QPushButton::clicked,
            this, &MainWindow::stopPlayback);
    connect(programList_, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { playSelectedProgram(); });

    connect(socket_, &QTcpSocket::connected,
            this, &MainWindow::onConnected);
    connect(socket_, &QTcpSocket::disconnected,
            this, &MainWindow::onDisconnected);
    connect(socket_, &QTcpSocket::readyRead,
            this, &MainWindow::onReadyRead);
    connect(socket_, &QAbstractSocket::errorOccurred,
            this, &MainWindow::onSocketError);
}

void MainWindow::updateUiState()
{
    const bool connected = socket_->state() == QAbstractSocket::ConnectedState;
    const bool connecting = socket_->state() == QAbstractSocket::ConnectingState;

    ipEdit_->setEnabled(!connected && !connecting);
    portSpinBox_->setEnabled(!connected && !connecting);
    connectButton_->setEnabled(!connected && !connecting);
    disconnectButton_->setEnabled(connected || connecting);
    refreshButton_->setEnabled(connected);
    playButton_->setEnabled(connected);
    stopButton_->setEnabled(connected);
}

void MainWindow::appendStatus(const QString &message)
{
    const QString time = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    statusText_->append(QString("[%1] %2").arg(time, message));
}

void MainWindow::sendCommand(const QString &command)
{
    QByteArray data;

    if (socket_->state() != QAbstractSocket::ConnectedState)
    {
        appendStatus(QString("Cannot send command while disconnected: %1").arg(command));
        return;
    }

    data = command.toUtf8();
    data.append('\n');

    socket_->write(data);
    appendStatus(QString(">> %1").arg(command));
}

void MainWindow::processLine(const QString &line)
{
    const QString prefix = "PROGRAM ";

    appendStatus(QString("<< %1").arg(line));

    if (line == "END")
        return;

    if (line.startsWith(prefix))
    {
        const QString payload = line.mid(prefix.size()).trimmed();
        const int firstSpace = payload.indexOf(' ');
        bool ok = false;
        int id = -1;
        QString name;

        if (firstSpace < 0)
        {
            appendStatus("Invalid PROGRAM line: missing name.");
            return;
        }

        id = payload.left(firstSpace).toInt(&ok);
        name = payload.mid(firstSpace + 1).trimmed();

        if (!ok || name.isEmpty())
        {
            appendStatus("Invalid PROGRAM line: parse failed.");
            return;
        }

        addProgramItem(id, name);
    }
}

void MainWindow::addProgramItem(int id, const QString &name)
{
    QListWidgetItem *item = new QListWidgetItem(
        QString("%1    %2").arg(id).arg(name),
        programList_);

    item->setData(Qt::UserRole, id);
}

int MainWindow::selectedProgramId() const
{
    QListWidgetItem *item = programList_->currentItem();

    if (item == nullptr)
        return -1;

    return item->data(Qt::UserRole).toInt();
}
