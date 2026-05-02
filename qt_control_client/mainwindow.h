#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QByteArray>
#include <QMainWindow>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;
class QTcpSocket;
class QTextEdit;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void connectToCore();
    void disconnectFromCore();
    void refreshProgramList();
    void playSelectedProgram();
    void stopPlayback();

    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError();

private:
    void setupUi();
    void setupConnections();
    void updateUiState();
    void appendStatus(const QString &message);
    void sendCommand(const QString &command);
    void processLine(const QString &line);
    void addProgramItem(int id, const QString &name);
    int selectedProgramId() const;

    QTcpSocket *socket_;
    QByteArray recvBuffer_;

    QLineEdit *ipEdit_;
    QSpinBox *portSpinBox_;
    QPushButton *connectButton_;
    QPushButton *disconnectButton_;
    QPushButton *refreshButton_;
    QPushButton *playButton_;
    QPushButton *stopButton_;
    QListWidget *programList_;
    QTextEdit *statusText_;
};

#endif
