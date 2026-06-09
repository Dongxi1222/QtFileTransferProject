#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QByteArray>

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void startListen();
    void connectToPeer();
    void chooseFile();
    void chooseDirectory();
    void chooseReceiveDirectory();
    void sendSelectedPath();
    void onNewConnection();
    void onReadyRead();

private:
    enum ReceiveState {
        ReadHeaderLength,
        ReadHeader,
        ReadPayload
    };

    QTcpServer *server;
    QTcpSocket *socket;

    QLineEdit *listenPortEdit;
    QLineEdit *remoteHostEdit;
    QLineEdit *remotePortEdit;
    QLineEdit *selectedPathEdit;
    QLineEdit *receiveDirEdit;

    QPushButton *listenButton;
    QPushButton *connectButton;
    QPushButton *fileButton;
    QPushButton *dirButton;
    QPushButton *recvDirButton;
    QPushButton *sendButton;

    QProgressBar *progressBar;
    QTextEdit *logEdit;

    QByteArray rxBuffer;
    ReceiveState receiveState;
    quint32 expectedHeaderLength;

    QFile currentOutputFile;
    quint64 currentFileSize;
    quint64 currentReceived;
    QString currentRelativePath;
    int receiveLoggedPercent;

    void buildUi();
    void setSocket(QTcpSocket *newSocket);
    void log(const QString &message);
    bool ensureConnected();

    QByteArray makeFileHeader(const QString &relativePath, quint64 fileSize) const;
    bool sendOneFile(const QString &absolutePath, const QString &relativePath);

    void resetReceiveState();
};

#endif // MAINWINDOW_H
