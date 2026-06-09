#include "mainwindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QCoreApplication>
#include <QDataStream>
#include <QStandardPaths>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QStringList>
#include <QtGlobal>


static const QDataStream::Version STREAM_VERSION = QDataStream::Qt_5_14;

static const quint32 MAGIC = 0x51544654; // "QTFT"
static const quint16 PROTOCOL_VERSION = 1;
static const quint8 TYPE_FILE = 1;
static const int BLOCK_SIZE = 64 * 1024;          // 每个分块 64KB
static const int SEND_TIMEOUT_MS = 30000;          // 大文件写入等待超时 30 秒
static const qint64 MAX_PENDING_BYTES = 4 * 1024 * 1024; // 待发送缓冲区超过约 4MB 后等待写出

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent),
      server(new QTcpServer(this)),
      socket(nullptr),
      receiveState(ReadHeaderLength),
      expectedHeaderLength(0),
      currentFileSize(0),
      currentReceived(0),
      receiveLoggedPercent(0)
{
    buildUi();
    connect(server, &QTcpServer::newConnection, this, &MainWindow::onNewConnection);
}

MainWindow::~MainWindow()
{
    if (currentOutputFile.isOpen()) {
        currentOutputFile.close();
    }
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Qt 跨平台网络文件传输工具"));
    resize(760, 520);

    listenPortEdit = new QLineEdit(QStringLiteral("45454"), this);
    remoteHostEdit = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    remotePortEdit = new QLineEdit(QStringLiteral("45454"), this);
    selectedPathEdit = new QLineEdit(this);
    receiveDirEdit = new QLineEdit(this);

    const QString defaultRecvDir =
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
            + QStringLiteral("/QtTransferReceived");
    receiveDirEdit->setText(defaultRecvDir);

    listenButton = new QPushButton(QStringLiteral("开始监听"), this);
    connectButton = new QPushButton(QStringLiteral("连接对端"), this);
    fileButton = new QPushButton(QStringLiteral("选择文件"), this);
    dirButton = new QPushButton(QStringLiteral("选择目录"), this);
    recvDirButton = new QPushButton(QStringLiteral("接收目录"), this);
    sendButton = new QPushButton(QStringLiteral("发送"), this);

    progressBar = new QProgressBar(this);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);

    logEdit = new QTextEdit(this);
    logEdit->setReadOnly(true);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QHBoxLayout *listenLayout = new QHBoxLayout;
    listenLayout->addWidget(new QLabel(QStringLiteral("本地监听端口:"), this));
    listenLayout->addWidget(listenPortEdit);
    listenLayout->addWidget(listenButton);
    mainLayout->addLayout(listenLayout);

    QHBoxLayout *connectLayout = new QHBoxLayout;
    connectLayout->addWidget(new QLabel(QStringLiteral("对端 IP:"), this));
    connectLayout->addWidget(remoteHostEdit);
    connectLayout->addWidget(new QLabel(QStringLiteral("端口:"), this));
    connectLayout->addWidget(remotePortEdit);
    connectLayout->addWidget(connectButton);
    mainLayout->addLayout(connectLayout);

    QHBoxLayout *pathLayout = new QHBoxLayout;
    pathLayout->addWidget(new QLabel(QStringLiteral("待发送路径:"), this));
    pathLayout->addWidget(selectedPathEdit);
    pathLayout->addWidget(fileButton);
    pathLayout->addWidget(dirButton);
    mainLayout->addLayout(pathLayout);

    QHBoxLayout *recvLayout = new QHBoxLayout;
    recvLayout->addWidget(new QLabel(QStringLiteral("保存到:"), this));
    recvLayout->addWidget(receiveDirEdit);
    recvLayout->addWidget(recvDirButton);
    mainLayout->addLayout(recvLayout);

    mainLayout->addWidget(sendButton);
    mainLayout->addWidget(progressBar);
    mainLayout->addWidget(logEdit);

    connect(listenButton, &QPushButton::clicked, this, &MainWindow::startListen);
    connect(connectButton, &QPushButton::clicked, this, &MainWindow::connectToPeer);
    connect(fileButton, &QPushButton::clicked, this, &MainWindow::chooseFile);
    connect(dirButton, &QPushButton::clicked, this, &MainWindow::chooseDirectory);
    connect(recvDirButton, &QPushButton::clicked, this, &MainWindow::chooseReceiveDirectory);
    connect(sendButton, &QPushButton::clicked, this, &MainWindow::sendSelectedPath);
}

void MainWindow::startListen()
{
    const quint16 port = listenPortEdit->text().toUShort();

    if (server->isListening()) {
        server->close();
    }

    if (server->listen(QHostAddress::Any, port)) {
        log(QStringLiteral("开始监听端口 %1。另一端可填写推荐 IP 进行连接。").arg(port));

        QStringList recommendedIps;
        QStringList otherIps;

        const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface &iface : interfaces) {
            const QNetworkInterface::InterfaceFlags flags = iface.flags();

            if (!(flags & QNetworkInterface::IsUp)
                    || !(flags & QNetworkInterface::IsRunning)
                    || (flags & QNetworkInterface::IsLoopBack)) {
                continue;
            }

            const QString ifaceInfo =
                    (iface.humanReadableName() + QStringLiteral(" ") + iface.name()).toLower();

            const bool isVirtualAdapter =
                    ifaceInfo.contains(QStringLiteral("vmware"))
                    || ifaceInfo.contains(QStringLiteral("virtualbox"))
                    || ifaceInfo.contains(QStringLiteral("vbox"))
                    || ifaceInfo.contains(QStringLiteral("vethernet"))
                    || ifaceInfo.contains(QStringLiteral("hyper-v"))
                    || ifaceInfo.contains(QStringLiteral("wsl"))
                    || ifaceInfo.contains(QStringLiteral("docker"))
                    || ifaceInfo.contains(QStringLiteral("bluetooth"))
                    || ifaceInfo.contains(QStringLiteral("loopback"));

            const QList<QNetworkAddressEntry> entries = iface.addressEntries();
            for (const QNetworkAddressEntry &entry : entries) {
                const QHostAddress address = entry.ip();

                if (address.protocol() != QAbstractSocket::IPv4Protocol
                        || address.isLoopback()) {
                    continue;
                }

                const QString ip = address.toString();

                // 跳过自动私有地址，通常不是可用局域网地址。
                if (ip.startsWith(QStringLiteral("169.254."))) {
                    continue;
                }

                if (isVirtualAdapter) {
                    otherIps << ip;
                } else {
                    recommendedIps << ip;
                }
            }
        }

        recommendedIps.removeDuplicates();
        otherIps.removeDuplicates();

        if (!recommendedIps.isEmpty()) {
            for (const QString &ip : recommendedIps) {
                log(QStringLiteral("推荐连接 IP: %1:%2").arg(ip).arg(port));
            }
        } else if (!otherIps.isEmpty()) {
            log(QStringLiteral("未检测到物理网卡 IPv4，下面列出可用 IPv4 地址："));
            for (const QString &ip : otherIps) {
                log(QStringLiteral("可用 IP: %1:%2").arg(ip).arg(port));
            }
        } else {
            log(QStringLiteral("未检测到非回环 IPv4 地址。本机双实例测试可使用 127.0.0.1:%1。").arg(port));
        }
    } else {
        log(QStringLiteral("监听失败: %1").arg(server->errorString()));
    }
}

void MainWindow::connectToPeer()
{
    QTcpSocket *newSocket = new QTcpSocket(this);
    setSocket(newSocket);

    socket->connectToHost(remoteHostEdit->text(), remotePortEdit->text().toUShort());

    if (socket->waitForConnected(3000)) {
        log(QStringLiteral("已连接到 %1:%2")
            .arg(remoteHostEdit->text())
            .arg(remotePortEdit->text()));
    } else {
        log(QStringLiteral("连接失败: %1").arg(socket->errorString()));
    }
}

void MainWindow::chooseFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择要发送的文件"));
    if (!path.isEmpty()) {
        selectedPathEdit->setText(path);
    }
}

void MainWindow::chooseDirectory()
{
    const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择要发送的目录"));
    if (!path.isEmpty()) {
        selectedPathEdit->setText(path);
    }
}

void MainWindow::chooseReceiveDirectory()
{
    const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择接收保存目录"));
    if (!path.isEmpty()) {
        receiveDirEdit->setText(path);
    }
}

void MainWindow::onNewConnection()
{
    QTcpSocket *newSocket = server->nextPendingConnection();
    setSocket(newSocket);

    log(QStringLiteral("收到来自 %1:%2 的连接")
        .arg(socket->peerAddress().toString())
        .arg(socket->peerPort()));
}

void MainWindow::setSocket(QTcpSocket *newSocket)
{
    if (socket && socket != newSocket) {
        socket->disconnect(this);
        socket->deleteLater();
    }

    socket = newSocket;

    connect(socket, &QTcpSocket::readyRead, this, &MainWindow::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, [this]() {
        log(QStringLiteral("连接已断开。"));
    });

    resetReceiveState();
}

bool MainWindow::ensureConnected()
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        log(QStringLiteral("尚未连接对端，请先监听并建立连接。"));
        return false;
    }
    return true;
}

QByteArray MainWindow::makeFileHeader(const QString &relativePath, quint64 fileSize) const
{
    QByteArray header;
    QDataStream out(&header, QIODevice::WriteOnly);
    out.setVersion(STREAM_VERSION);

    out << MAGIC
        << PROTOCOL_VERSION
        << TYPE_FILE
        << relativePath
        << fileSize;

    return header;
}

bool MainWindow::sendOneFile(const QString &absolutePath, const QString &relativePath)
{
    if (!ensureConnected()) return false;

    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) {
        log(QStringLiteral("无法打开文件: %1").arg(absolutePath));
        return false;
    }

    const quint64 totalSize = static_cast<quint64>(file.size());
    const quint64 totalBlocks = totalSize == 0 ? 0 : (totalSize + BLOCK_SIZE - 1) / BLOCK_SIZE;

    log(QStringLiteral("开始分块发送: %1，文件大小 %2 bytes，分块大小 %3 bytes，预计 %4 块")
        .arg(relativePath).arg(totalSize).arg(BLOCK_SIZE).arg(totalBlocks));

    const QByteArray header = makeFileHeader(relativePath, totalSize);

    QByteArray prefix;
    QDataStream prefixOut(&prefix, QIODevice::WriteOnly);
    prefixOut.setVersion(STREAM_VERSION);
    prefixOut << static_cast<quint32>(header.size());

    socket->write(prefix);
    socket->write(header);

    quint64 sent = 0;
    quint64 blockIndex = 0;
    int lastLoggedPercent = -1;

    while (!file.atEnd()) {
        const QByteArray block = file.read(BLOCK_SIZE);
        blockIndex++;

        if (socket->write(block) < 0) {
            log(QStringLiteral("发送失败: %1").arg(socket->errorString()));
            file.close();
            return false;
        }

        if (socket->bytesToWrite() > MAX_PENDING_BYTES) {
            if (!socket->waitForBytesWritten(SEND_TIMEOUT_MS)) {
                log(QStringLiteral("写入网络超时或失败: %1").arg(socket->errorString()));
                file.close();
                return false;
            }
        }

        sent += static_cast<quint64>(block.size());
        int percent = 100;
        if (totalSize > 0) {
            percent = static_cast<int>(sent * 100 / totalSize);
            progressBar->setValue(percent);
        }

        if (percent == 0 || percent == 100 || percent >= lastLoggedPercent + 10) {
            lastLoggedPercent = percent;
            log(QStringLiteral("分块发送进度: 第 %1/%2 块，已发送 %3 bytes，进度 %4%")
                .arg(blockIndex).arg(totalBlocks).arg(sent).arg(percent));
        }

        QCoreApplication::processEvents();
    }

    while (socket->bytesToWrite() > 0) {
        if (!socket->waitForBytesWritten(SEND_TIMEOUT_MS)) {
            log(QStringLiteral("发送收尾阶段超时或失败: %1").arg(socket->errorString()));
            file.close();
            return false;
        }
        QCoreApplication::processEvents();
    }

    socket->flush();
    file.close();

    log(QStringLiteral("发送完成: %1 (%2 bytes，共 %3 个分块)")
        .arg(relativePath).arg(totalSize).arg(totalBlocks));
    return true;
}

void MainWindow::sendSelectedPath()
{
    const QString path = selectedPathEdit->text().trimmed();

    if (path.isEmpty()) {
        log(QStringLiteral("请先选择文件或目录。"));
        return;
    }

    if (!ensureConnected()) {
        return;
    }

    QFileInfo info(path);
    progressBar->setValue(0);

    if (info.isFile()) {
        sendOneFile(path, info.fileName());
    } else if (info.isDir()) {
        QDir root(path);
        QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);

        int count = 0;
        while (it.hasNext()) {
            const QString filePath = it.next();
            const QString relativePath = root.relativeFilePath(filePath);
            sendOneFile(filePath, relativePath);
            count++;
        }

        log(QStringLiteral("目录发送完成，共发送 %1 个文件。").arg(count));
    } else {
        log(QStringLiteral("路径无效: %1").arg(path));
    }
}

void MainWindow::resetReceiveState()
{
    rxBuffer.clear();
    receiveState = ReadHeaderLength;
    expectedHeaderLength = 0;
    currentFileSize = 0;
    currentReceived = 0;
    currentRelativePath.clear();
    receiveLoggedPercent = 0;

    if (currentOutputFile.isOpen()) {
        currentOutputFile.close();
    }
}

void MainWindow::onReadyRead()
{
    rxBuffer.append(socket->readAll());

    while (true) {
        if (receiveState == ReadHeaderLength) {
            if (rxBuffer.size() < static_cast<int>(sizeof(quint32))) {
                break;
            }

            QByteArray lenBytes = rxBuffer.left(sizeof(quint32));
            rxBuffer.remove(0, sizeof(quint32));

            QDataStream lenStream(lenBytes);
            lenStream.setVersion(STREAM_VERSION);
            lenStream >> expectedHeaderLength;

            receiveState = ReadHeader;
        }

        if (receiveState == ReadHeader) {
            if (rxBuffer.size() < static_cast<int>(expectedHeaderLength)) {
                break;
            }

            QByteArray header = rxBuffer.left(expectedHeaderLength);
            rxBuffer.remove(0, expectedHeaderLength);

            QDataStream in(header);
            in.setVersion(STREAM_VERSION);

            quint32 magic;
            quint16 protocolVersion;
            quint8 type;

            in >> magic
               >> protocolVersion
               >> type
               >> currentRelativePath
               >> currentFileSize;

            if (magic != MAGIC || protocolVersion != PROTOCOL_VERSION || type != TYPE_FILE) {
                log(QStringLiteral("协议头错误，已重置接收状态。"));
                resetReceiveState();
                break;
            }

            QDir receiveDir(receiveDirEdit->text());
            if (!receiveDir.exists()) {
                receiveDir.mkpath(QStringLiteral("."));
            }

            const QString outputPath = receiveDir.filePath(currentRelativePath);
            QDir parentDir = QFileInfo(outputPath).dir();

            if (!parentDir.exists()) {
                parentDir.mkpath(QStringLiteral("."));
            }

            if (currentOutputFile.isOpen()) {
                currentOutputFile.close();
            }

            currentOutputFile.setFileName(outputPath);

            if (!currentOutputFile.open(QIODevice::WriteOnly)) {
                log(QStringLiteral("无法创建接收文件: %1").arg(outputPath));
                resetReceiveState();
                break;
            }

            currentReceived = 0;
            progressBar->setValue(0);

            const quint64 receiveBlocks = currentFileSize == 0 ? 0 : (currentFileSize + BLOCK_SIZE - 1) / BLOCK_SIZE;
            log(QStringLiteral("开始接收: %1 (%2 bytes，分块大小 %3 bytes，预计 %4 块)")
                .arg(currentRelativePath).arg(currentFileSize).arg(BLOCK_SIZE).arg(receiveBlocks));
            receiveLoggedPercent = 0;

            if (currentFileSize == 0) {
                currentOutputFile.close();
                log(QStringLiteral("接收完成: %1").arg(currentRelativePath));
                receiveState = ReadHeaderLength;
            } else {
                receiveState = ReadPayload;
            }
        }

        if (receiveState == ReadPayload) {
            if (rxBuffer.isEmpty()) {
                break;
            }

            const quint64 remaining = currentFileSize - currentReceived;
            const qint64 writeSize = qMin<qint64>(
                        static_cast<qint64>(remaining),
                        static_cast<qint64>(rxBuffer.size()));

            if (writeSize <= 0) {
                break;
            }

            currentOutputFile.write(rxBuffer.constData(), writeSize);
            rxBuffer.remove(0, static_cast<int>(writeSize));
            currentReceived += static_cast<quint64>(writeSize);

            if (currentFileSize > 0) {
                const int percent = static_cast<int>(currentReceived * 100 / currentFileSize);
                progressBar->setValue(percent);
                if (percent == 100 || percent >= receiveLoggedPercent + 10) {
                    receiveLoggedPercent = percent;
                    log(QStringLiteral("接收进度: 已接收 %1/%2 bytes，进度 %3%")
                        .arg(currentReceived).arg(currentFileSize).arg(percent));
                }
            }

            if (currentReceived >= currentFileSize) {
                currentOutputFile.close();
                log(QStringLiteral("接收完成: %1").arg(currentRelativePath));

                receiveState = ReadHeaderLength;
                expectedHeaderLength = 0;
                currentFileSize = 0;
                currentReceived = 0;
                currentRelativePath.clear();
    receiveLoggedPercent = 0;
            }
        }
    }
}

void MainWindow::log(const QString &message)
{
    logEdit->append(QStringLiteral("[%1] %2")
                    .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
                    .arg(message));
}
