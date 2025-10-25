/*
*  Project+ Dolphin Self-Updater
*  Credit to the Mario Party Netplay team for the base code of this updater
*  Copyright (C) 2025 Tabitha Hanegan
*/

#include "Common/MinizipUtil.h"
#include "InstallUpdateDialog.h"
#include "DownloadWorker.h"

#ifdef __APPLE__
#include <unistd.h>
#endif

#include <QDirIterator>
#include <QCoreApplication>
#include <QProcess>
#include <QDir>
#include <QTextStream>
#include <QVBoxLayout>
#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QMessageBox>
#include <QThread>
#include <QStorageInfo>
#include <QJsonObject>
#include <QTimer>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QDebug>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QUrl>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include "Common/HttpRequest.h"

#include <mz.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

// --------------------- SCOOP DEPENDENCY CHECK ----------------------
void InstallUpdateDialog::ensureDependenciesThen(std::function<void()> cont)
{
#ifdef _WIN32
    const bool haveAria   = !QStandardPaths::findExecutable(QStringLiteral("aria2c")).isEmpty();
    const bool haveRclone = !QStandardPaths::findExecutable(QStringLiteral("rclone")).isEmpty();

    if (haveAria && haveRclone) {
        // Nothing to do
        cont();
        return;
    }

    // Tell the user what we’re doing (non-blocking)
    stepLabel->setText(QStringLiteral("Installing download tools (scoop)..."));
    qDebug().noquote() << "[deps] Installing aria2 & rclone via scoop (if missing)";

    QProcess* ps = new QProcess(this);
    ps->setProcessChannelMode(QProcess::MergedChannels);
    const QString psPath = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));

    // User-scope install (no admin), add bucket, install missing tools.
    // Always continue even if something fails so we still try rclone/HTTP later.
    const QString script = QString::fromUtf8(
        "$ErrorActionPreference='Continue';"
        "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;"
        "if (-not (Get-Command scoop -ErrorAction SilentlyContinue)) {"
        "  iwr -useb get.scoop.sh | iex"
        "};"
        "scoop bucket add main 2>$null | Out-Null;"
        "if (-not (Get-Command aria2c -ErrorAction SilentlyContinue)) {"
        "  scoop install aria2 2>$null | Out-Null"
        "};"
        "if (-not (Get-Command rclone -ErrorAction SilentlyContinue)) {"
        "  scoop install rclone 2>$null | Out-Null"
        "};"
        "Write-Host 'DONE'"
    );

    connect(ps, &QProcess::readyReadStandardOutput, this, [this, ps]() {
        const QString out = QString::fromUtf8(ps->readAllStandardOutput()).trimmed();
        if (!out.isEmpty())
            qDebug().noquote() << "[scoop]" << out;
    });

    connect(ps, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [=](int /*code*/, QProcess::ExitStatus) {
        ps->deleteLater();
        // Re-check and continue regardless — we still have rclone/HTTP fallbacks
        const bool nowAria   = !QStandardPaths::findExecutable(QStringLiteral("aria2c")).isEmpty();
        const bool nowRclone = !QStandardPaths::findExecutable(QStringLiteral("rclone")).isEmpty();
        qDebug().noquote() << "[deps] aria2c=" << nowAria << "rclone=" << nowRclone;
        cont();
    });

    ps->start(psPath.isEmpty() ? QStringLiteral("powershell.exe") : psPath,
              { QStringLiteral("-NoProfile"),
                QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                QStringLiteral("-Command"), script });
#else
    // Non-Windows: nothing to install here — just continue.
    cont();
#endif
}



// --------------------- UTILITAIRE ETA ----------------------
static QString formatETA(double seconds)
{
    if (seconds < 1.0)
        return QStringLiteral("less than 1s");

    int m = static_cast<int>(seconds) / 60;
    int s = static_cast<int>(seconds) % 60;

    if (m > 0)
    {
        QString sStr = QString::asprintf("%02d", s);
        return QString::asprintf("%dm%ss", m, sStr.toUtf8().constData());
    }

    return QString::asprintf("%ds", s);
}

// --------------------- UTILITAIRES -----------------------
static bool hasEnoughFreeSpace(const QString& path, qint64 minFreeBytes)
{
    QStorageInfo storage(path);
    storage.refresh();
    return storage.bytesAvailable() >= minFreeBytes;
}

// --------------------- CONSTRUCTEUR ----------------------
InstallUpdateDialog::InstallUpdateDialog(QWidget *parent,
                                         QString installationDirectory,
                                         QString temporaryDirectory,
                                         QString filename,
                                         QString downloadUrl,
                                         QString sdUrl)
    : QDialog(parent),
      installationDirectory(std::move(installationDirectory)),
      temporaryDirectory(std::move(temporaryDirectory)),
      filename(std::move(filename)),
      downloadUrl(std::move(downloadUrl)),
      m_sdUrl(std::move(sdUrl))
{
    setWindowTitle(QStringLiteral("Project+ Dolphin - Updater"));

    QVBoxLayout* layout = new QVBoxLayout(this);
    label = new QLabel(QStringLiteral("Preparing installation..."), this);
    progressBar = new QProgressBar(this);
    stepLabel = new QLabel(QStringLiteral("Preparing..."), this);
    stepProgressBar = new QProgressBar(this);

    layout->addWidget(label);
    layout->addWidget(progressBar);
    layout->addWidget(stepLabel);
    layout->addWidget(stepProgressBar);
    setLayout(layout);
    setMinimumSize(400, 150);

    progressBar->setVisible(true);
    stepLabel->setVisible(true);
    stepProgressBar->setVisible(true);

    startTimer(100);
}

InstallUpdateDialog::~InstallUpdateDialog() = default;

// --------------------- CHECK ----------------------
void InstallUpdateDialog::checkIfAllDownloadsFinished(bool sdFinished, bool sdSuccess)
{
    static bool sdDone = false;
    static bool zipStarted = false;
    static bool zipDone = false;

    // Mise à jour des états
    if (sdFinished && sdSuccess)
        sdDone = true;

    qDebug().noquote() << "🧠 Status → sdDone=" << sdDone
                       << ", zipStarted=" << zipStarted
                       << ", zipDone=" << zipDone;

    // ------------- ETAPE 1 : SD pas encore finie -------------
    if (!sdDone)
    {
        qDebug().noquote() << "⏳ Waiting for SD download...";
        return;
    }

    // ------------- ETAPE 2 : Téléchargement ZIP -------------
    if (!zipStarted && !downloadUrl.isEmpty())
    {
        zipStarted = true;
        qDebug().noquote() << "📦 Starting main ZIP download after SD...";

        // Mise à jour UI
        label->setText(QStringLiteral("Step 2/2: Downloading main package..."));
        stepLabel->setText(QStringLiteral("Preparing download..."));
        stepProgressBar->setValue(0);
        progressBar->setValue(50);  // ✅ 50 % après la SD

        QThread* thread = new QThread;

       #ifdef __APPLE__

    // 📦 Corrige la destination du téléchargement sur macOS
    QString appDir = QCoreApplication::applicationDirPath();     // .../Contents/MacOS
    QString contentsDir = QFileInfo(appDir).path();              // .../Contents
    QString appBundleDir = QFileInfo(contentsDir).path();        // .../ProjectPlusFR.app
    QString parentDir = QFileInfo(appBundleDir).path();          // dossier parent du .app

    QString realTmpDir = QDir(parentDir).filePath(QStringLiteral("update_tmp"));
    QDir().mkpath(realTmpDir); // s'assurer que le dossier existe

    QString zipPath = QDir(realTmpDir).filePath(filename);
#else
    // 🪟 / 🐧 Windows & Linux : forcer le ZIP dans update_tmp à côté de Dolphin.exe
    QString appDir = QCoreApplication::applicationDirPath();
    QDir dir(appDir);

    // Remonte jusqu’à portable.txt pour les versions portables
    while (!dir.isRoot() && !QFile::exists(dir.filePath(QStringLiteral("portable.txt"))))
        dir.cdUp();

    QString baseDir = QFile::exists(dir.filePath(QStringLiteral("portable.txt")))
                          ? dir.absolutePath()
                          : QFileInfo(installationDirectory).path();

    QString realTmpDir = QDir(baseDir).filePath(QStringLiteral("update_tmp"));
    QDir().mkpath(realTmpDir);

    QString zipPath = QDir(realTmpDir).filePath(filename);
    qDebug().noquote() << "📦 [Windows] Download ZIP path forced to:" << zipPath;
#endif

qDebug().noquote() << "🚀 curl (or worker) will download ZIP to:" << zipPath;

auto* worker = new DownloadWorker(downloadUrl, zipPath);

        worker->moveToThread(thread);

        connect(thread, &QThread::started, worker, &DownloadWorker::startDownload); 

        static double zipDisplayed = 50.0;  // valeur affichée
static double zipTarget    = 50.0;  // cible à atteindre
static QTimer* zipAnimTimer = nullptr;

// Démarre l’anim une seule fois
if (!zipAnimTimer)
{
    zipAnimTimer = new QTimer(this);
    zipAnimTimer->setInterval(30); // ~33 FPS
    connect(zipAnimTimer, &QTimer::timeout, this, [this]() {
    // Empêche tout retour en arrière brutal
    if (zipTarget < zipDisplayed)
        zipTarget = zipDisplayed;

    // Lissage fluide vers la cible
    zipDisplayed += (zipTarget - zipDisplayed) * 0.15;

    // Clamp et mise à jour UI
    int value = qBound(0, static_cast<int>(zipDisplayed), 100);
    progressBar->setValue(value);
});
    zipAnimTimer->start();
}

// valeur de départ à 50% quand on commence l’étape 2
zipDisplayed = 50.0;
zipTarget    = 50.0;
progressBar->setValue(50);

      connect(worker, &DownloadWorker::progressUpdated, this, [this](qint64 done, qint64 total) {
    if (total <= 0) return;

    const int localPercent = static_cast<int>((done * 100) / total);

    // barre d’étape (celle du bas) = progression du ZIP
    stepProgressBar->setValue(localPercent);
    stepLabel->setText(QStringLiteral("Step 2/2: Downloading ZIP (%1%)").arg(localPercent));

    // barre globale (haut) : cible entre 50 et 100
    // 50 + (0..100)*0.5  =>  50..100
    zipTarget = 50.0 + (static_cast<double>(localPercent) * 0.5);
});

       connect(worker, &DownloadWorker::finished, this, [=]() {
    qDebug().noquote() << "✅ Main ZIP download finished";
    stepLabel->setText(QStringLiteral("ZIP download complete"));
    stepProgressBar->setValue(75);

    // 💫 Amène la cible à 100% (la barre globale va s’animer jusqu’à 100)
    zipTarget = 100.0;

    // Sécurité : après 400 ms, on s’assure qu’elle est bien à 100%
    QTimer::singleShot(400, this, [this]() {
      if (m_isClosing) return; // ✅ sécurité
        progressBar->setValue(100);
    });

    zipDone = true;

    thread->quit();
    worker->deleteLater();
    thread->deleteLater();

    // ✅ Passage à l’installation après un petit délai (le temps que la barre termine son anim)
    QTimer::singleShot(500, this, [this]() {
      if (m_isClosing) return; // ✅ sécurité
        this->checkIfAllDownloadsFinished(true, true);
    });
});


        connect(worker, &DownloadWorker::errorOccurred, this, [=](const QString& err) {
            qWarning().noquote() << "❌ ZIP download failed:" << err;
            zipDone = true;
            thread->quit();
            worker->deleteLater();
            thread->deleteLater();
            QMessageBox::critical(this, QStringLiteral("Error"), err);
        });

        thread->start();
        return;
    }

    // ------------- ETAPE 3 : Installation -------------
    if (sdDone && zipDone)
    {
        qDebug().noquote() << "🚀 All downloads complete → starting installation...";
        label->setText(QStringLiteral("Installing update..."));
        progressBar->setValue(100);
        stepLabel->setText(QStringLiteral("Extracting and finalizing..."));
        install();
    }
}


void InstallUpdateDialog::download()
{
  const qint64 minRequiredBytes = 8ll * 1024 * 1024 * 1024; // 8 Go
if (!hasEnoughFreeSpace(installationDirectory, minRequiredBytes))
{
    QMessageBox::critical(this,
        QStringLiteral("Not enough space"),
        QStringLiteral("You need at least 8 GB of free space to install this update.\n\n"
                       "Please free some disk space and try again."));
    qWarning().noquote() << "❌ Not enough free space. Aborting update.";
    reject();
    return;
}

    label->setText(QStringLiteral("Step 1/2: Checking SD card..."));
    progressBar->setRange(0, 100);
    stepProgressBar->setRange(0, 100);
    progressBar->setValue(0);
    stepProgressBar->setValue(0);
    stepLabel->setText(QStringLiteral("Checking local SD hash..."));

    // 🧩 Détection du dossier "portable.txt" pour macOS/Linux, safe pour Windows
QString baseDir = QCoreApplication::applicationDirPath();
QDir dir(baseDir);

// Remonte jusqu’à trouver portable.txt (ou la racine)
while (!dir.isRoot() && !QFile::exists(dir.filePath(QStringLiteral("portable.txt")))) {
    dir.cdUp();
}

// Si on a trouvé portable.txt → on redéfinit la base
if (QFile::exists(dir.filePath(QStringLiteral("portable.txt")))) {
    baseDir = dir.absolutePath();
}

const QString sdPath = QDir::toNativeSeparators(baseDir + QStringLiteral("/User/Wii/sd.raw"));

    // ✅ Si la SD existe localement → calcul hash dans un thread
    if (QFile::exists(sdPath))
    {
        qDebug().noquote() << "🔍 SD.raw found locally at:" << sdPath;

        stepLabel->setText(QStringLiteral("Computing SD hash..."));
        stepProgressBar->setRange(0, 0); // indéterminée

        // ⚙️ Thread de calcul du hash
        m_hashThread = QThread::create([this, sdPath]() {
            QFile sdFile(sdPath);
            QString localHash;
            if (sdFile.open(QIODevice::ReadOnly))
            {
                QCryptographicHash hash(QCryptographicHash::Sha256);
                constexpr qint64 chunkSize = 4 * 1024 * 1024; // 4 Mo
                QByteArray buffer;
                buffer.resize(chunkSize);

                qint64 totalSize = sdFile.size();
                qint64 sampleSize = 256ll * 1024 * 1024; // 256 Mo

                // --- Début du fichier ---
                sdFile.seek(0);
                qint64 bytesRead = 0;
                while (bytesRead < sampleSize && !sdFile.atEnd())
                {
                    qint64 n = sdFile.read(buffer.data(), chunkSize);
                    if (n <= 0) break;
                    hash.addData(buffer.constData(), n);
                    bytesRead += n;
                }

                // --- Fin du fichier ---
                if (totalSize > sampleSize)
                {
                    sdFile.seek(qMax(0ll, totalSize - sampleSize));
                    bytesRead = 0;
                    while (!sdFile.atEnd() && bytesRead < sampleSize)
                    {
                        qint64 n = sdFile.read(buffer.data(), chunkSize);
                        if (n <= 0) break;
                        hash.addData(buffer.constData(), n);
                        bytesRead += n;
                    }
                }

                localHash = QString::fromLatin1(hash.result().toHex());
                sdFile.close();
            }

            // ✅ Vérifie que la fenêtre est encore ouverte
            if (m_isClosing)
                return;

            // 🔹 Retour au thread principal
            QMetaObject::invokeMethod(QApplication::instance(), [this, localHash]() {
                if (m_isClosing)
                    return; // sécurité

                qDebug().noquote() << "💠 Local SD hash =" << localHash;
                stepProgressBar->setRange(0, 100);
                stepProgressBar->setValue(100);
                stepLabel->setText(QStringLiteral("Hash check complete!"));

                // 🔹 Compare avec le hash distant
                const QString hashUrl = QStringLiteral("https://update.pplusfr.org/update2.json");
                Common::HttpRequest req;
                auto response = req.Get(hashUrl.toStdString());

                if (!response.has_value())
                {
                    qWarning().noquote() << "⚠️ Failed to fetch" << hashUrl;
                    stepLabel->setText(QStringLiteral("⚠️ Failed to fetch update.json"));
                    return;
                }

                const QByteArray jsonBytes(reinterpret_cast<const char*>(response->data()),
                                           static_cast<int>(response->size()));
                const QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonBytes);
                if (!jsonDoc.isObject())
                {
                    qWarning().noquote() << "⚠️ Invalid JSON from" << hashUrl;
                    return;
                }

                const QJsonObject obj = jsonDoc.object();
                const QString remoteHash = obj.value(QStringLiteral("sd-hash-partial")).toString();

                qDebug().noquote() << "🌐 Remote SD hash (partial) =" << remoteHash;

                if (!remoteHash.isEmpty() &&
                    QString::compare(remoteHash, localHash, Qt::CaseInsensitive) == 0)
                {
                    qDebug().noquote() << "✅ SD already up to date, skipping SD download.";
                    progressBar->setValue(50);
                    stepProgressBar->setValue(100);
                    stepLabel->setText(QStringLiteral("SD is up to date — skipping download"));
                    QTimer::singleShot(300, this, [this]() {
                        if (m_isClosing) return;
                        this->checkIfAllDownloadsFinished(true, true);
                    });
                    return;
                }

                qDebug().noquote() << "⚠️ SD outdated — re-downloading...";
                stepLabel->setText(QStringLiteral("Downloading new SD..."));
                progressBar->setValue(25);
                stepProgressBar->setValue(0);

                this->startSDDownload();
            });
        });

        // 🔹 Nettoyage du thread si la fenêtre est fermée
        connect(this, &QObject::destroyed, this, [this]() {
            if (m_hashThread && m_hashThread->isRunning()) {
                qDebug().noquote() << "🧵 Stopping hash thread (window closed)";
                m_hashThread->requestInterruption();
                m_hashThread->quit();
                m_hashThread->wait(1000);
                m_hashThread = nullptr;
            }
        });

        m_hashThread->start();
        return;
    }

    // -------------------- Aucun fichier SD local → téléchargement direct --------------------
    qDebug().noquote() << "⚠️ No SD.raw found locally → will download.";
    label->setText(QStringLiteral("Step 1/2: Downloading SD card..."));
    stepLabel->setText(QStringLiteral("0% Downloaded..."));
    progressBar->setValue(0);
    stepProgressBar->setValue(0);

    this->startSDDownload(); // télécharge directement
}




void InstallUpdateDialog::startSDDownload()
{
    // Vérifie que aria2c/rclone sont prêts avant de lancer
    ensureDependenciesThen([=]() {
        const QString sdUrl = m_sdUrl;
        if (sdUrl.isEmpty())
        {
            qWarning().noquote() << "⚠️ No SD URL found, skipping SD download.";
            checkIfAllDownloadsFinished(true, true);
            return;
        }

        // 🧩 Détection du dossier "portable.txt" pour macOS/Linux, safe pour Windows
QString baseDir = QCoreApplication::applicationDirPath();
QDir dir(baseDir);

// Remonte jusqu’à trouver portable.txt (ou la racine)
while (!dir.isRoot() && !QFile::exists(dir.filePath(QStringLiteral("portable.txt")))) {
    dir.cdUp();
}

// Si on a trouvé portable.txt → on redéfinit la base
if (QFile::exists(dir.filePath(QStringLiteral("portable.txt")))) {
    baseDir = dir.absolutePath();
}

const QString sdPath = QDir::toNativeSeparators(baseDir + QStringLiteral("/User/Wii/sd.raw"));

        QFileInfo fi(sdPath);
        QDir().mkpath(fi.path());
        if (QFile::exists(sdPath))
        {
            QFile::remove(sdPath);
            qDebug().noquote() << "🧹 Removed old file:" << sdPath;
        }

        auto sdFinished = std::make_shared<bool>(false);
        auto sdSuccess  = std::make_shared<bool>(false);

        auto uiProgress = [this](int p, const QString& t)
        {
            const int clamped = qBound(0, p, 100);
            stepProgressBar->setValue(clamped);
            stepLabel->setText(t);
           static int lastMain = -1;
if (clamped / 5 != lastMain / 5) {
    progressBar->setValue(clamped / 2);
    lastMain = clamped;
}
 };

        auto uiDone = [this](bool ok, const QString& t)
        {
            stepProgressBar->setValue(ok ? 100 : 0);
            stepLabel->setText(t);
            if (ok) progressBar->setValue(50);
        };

        // 1) aria2c si dispo
        const QString ariaPath = QStandardPaths::findExecutable(QStringLiteral("aria2c"));
        if (!ariaPath.isEmpty())
        {
            qDebug().noquote() << "🧩 aria2c detected → using aria2c for SD download";

            QProcess* aria = new QProcess(this);
            aria->setProcessChannelMode(QProcess::MergedChannels);
            aria->setWorkingDirectory(QFileInfo(sdPath).path());

            QStringList args = {
                QStringLiteral("--allow-overwrite=true"),
                QStringLiteral("-x"), QStringLiteral("8"),
                QStringLiteral("-s"), QStringLiteral("8"),
                QStringLiteral("--console-log-level=notice"),
                QStringLiteral("--summary-interval=1"),
                QStringLiteral("--enable-color=false"),
                QStringLiteral("--show-console-readout=false"),
                QStringLiteral("-d"), QFileInfo(sdPath).path(),
                QStringLiteral("-o"), QFileInfo(sdPath).fileName(),
                sdUrl
            };

            connect(aria, &QProcess::readyReadStandardOutput, this, [this, aria, uiProgress]() {
                const QString out = QString::fromUtf8(aria->readAllStandardOutput());
                QRegularExpression re(QStringLiteral(R"(\((\d{1,3})%\).+DL:([\d\.]+[KMG]i?B))"));
                auto m = re.match(out);
                if (m.hasMatch())
                {
                    int percent = qBound(0, m.captured(1).toInt(), 100);
                    const QString speed = m.captured(2).trimmed();
                    uiProgress(percent, QStringLiteral("aria2c: %1% (%2/s)").arg(percent).arg(speed));
                }
            });

            connect(aria, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                    this, [=](int code, QProcess::ExitStatus) {
                QFileInfo fi(sdPath);
                const bool ok = (code == 0 && fi.exists() && fi.size() > (10 * 1024 * 1024));

                if (ok)
                {
                    qDebug().noquote() << "✅ SD download succeeded with aria2c";
                    *sdFinished = true; *sdSuccess = true;
                    uiDone(true, QStringLiteral("🎉 SD download complete (aria2c)!"));
                    QMetaObject::invokeMethod(this, [=]() {
                        this->checkIfAllDownloadsFinished(*sdFinished, *sdSuccess);
                    }, Qt::QueuedConnection);
                }
                else
                {
                    qWarning().noquote() << "❌ aria2c failed → switching to rclone...";
                    startRcloneFallback(sdUrl, sdPath, sdFinished, sdSuccess, uiProgress, uiDone);
                }

                aria->deleteLater();
            });

            qDebug().noquote() << "🚀 Launching aria2c:" << ariaPath << args.join(QLatin1Char(' '));
            aria->start(ariaPath, args);
            return;
        }

        static int ariaRetryCount = 0;
if (ariaRetryCount < 1) {
    ariaRetryCount++;
    qWarning().noquote() << "⚠️ aria2c failed — retrying once...";
    QTimer::singleShot(1000, this, [=]() {
        startSDDownload(); // retente
    });
    return;
}

        // 2) rclone sinon (et il fallback vers HTTPS dans ta fonction)
        const QString rclonePath = QStandardPaths::findExecutable(QStringLiteral("rclone"));
        if (!rclonePath.isEmpty())
        {
            qDebug().noquote() << "🧩 rclone detected → using rclone fallback";
            startRcloneFallback(sdUrl, sdPath, sdFinished, sdSuccess, uiProgress, uiDone);
        }
        else
        {
            // 3) HTTP direct si rien
            qWarning().noquote() << "⚠️ Neither aria2c nor rclone found → switching to HTTPS fallback...";
            startHttpFallback(sdUrl, sdPath, sdFinished, sdSuccess, uiProgress, uiDone);
        }
    });
}


// --------------------- RCLONE FALLBACK (corrigé et optimisé) ----------------------
void InstallUpdateDialog::startRcloneFallback(const QString& sdUrl,
                                              const QString& sdPath,
                                              std::shared_ptr<bool> sdFinished,
                                              std::shared_ptr<bool> sdSuccess,
                                              std::function<void(int, const QString&)> uiProgress,
                                              std::function<void(bool, const QString&)> uiDone)
{
    QProcess* rclone = new QProcess(this);
    rclone->setProcessChannelMode(QProcess::MergedChannels);

    // 🔹 Extraire base URL et nom du fichier
    QUrl url(sdUrl);
    QString baseUrl = url.adjusted(QUrl::RemoveFilename).toString();
    QString fileName = QFileInfo(url.path()).fileName();

    // ⚙️ Commande rclone optimisée
    QStringList rargs = {
    QStringLiteral("copyto"),
    QStringLiteral(":http:%1").arg(fileName),
    QDir::toNativeSeparators(sdPath),
    QStringLiteral("--http-url"), baseUrl,
    QStringLiteral("--multi-thread-streams=4"),
    QStringLiteral("--multi-thread-cutoff=16M"),
    QStringLiteral("--buffer-size=128M"),
    QStringLiteral("--transfers=2"),
    QStringLiteral("--low-level-retries=5"),
    QStringLiteral("--checkers=4"),
    QStringLiteral("--retries=3"),
    QStringLiteral("--progress")
};

    qDebug().noquote() << "🚀 Launching rclone:" << rargs.join(QLatin1Char(' '));

    auto lastUpdate = std::make_shared<QElapsedTimer>();
    lastUpdate->start();

    connect(rclone, &QProcess::readyReadStandardOutput, this,
            [this, rclone, uiProgress, lastUpdate]() {
        const QString out = QString::fromUtf8(rclone->readAllStandardOutput());
        const QStringList lines = out.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);

        static int lastPercent = 0;

        for (const QString& line : lines)
        {
            QString trimmed = line.trimmed();

            // Exemple : "Transferred:   3.750 GiB / 5.500 GiB, 68%, 98.2 MiB/s, ETA 37s"
            QRegularExpression re(QStringLiteral(
                R"(Transferred:.*?,\s*(\d{1,3})%,\s*([\d\.]+\s*[KMG]i?B\/s))"));
            auto match = re.match(trimmed);

            if (match.hasMatch())
            {
                int percent = qBound(0, match.captured(1).toInt(), 100);
                QString speed = match.captured(2).trimmed();

                if (lastUpdate->elapsed() > 200 || percent != lastPercent)
                {
                    uiProgress(percent, QStringLiteral("rclone: %1% (%2)").arg(percent).arg(speed));
                    lastPercent = percent;
                    lastUpdate->restart();
                }
            }
        }
    });

    connect(rclone, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [=](int code, QProcess::ExitStatus) {
        QFileInfo fi(sdPath);
        const bool ok = (code == 0 && fi.exists() && fi.size() > (10 * 1024 * 1024));

        if (ok)
        {
            qDebug().noquote() << "✅ SD download succeeded with rclone";
            *sdFinished = true;
            *sdSuccess = true;
            uiDone(true, QStringLiteral("🎉 SD download complete (rclone)!"));
        }
        else
        {
            qWarning().noquote() << "❌ rclone failed → trying HTTPS fallback...";
            *sdFinished = false;
            *sdSuccess = false;

            // 🔁 Fallback HTTPS
            startHttpFallback(sdUrl, sdPath, sdFinished, sdSuccess, uiProgress, uiDone);
        }

        // ✅ Signale la fin (même si fallback)
        QMetaObject::invokeMethod(this, [=]() {
            this->checkIfAllDownloadsFinished(*sdFinished, *sdSuccess);
        }, Qt::QueuedConnection);

        rclone->deleteLater();
    });

    rclone->start(QStringLiteral("rclone"), rargs);
}




// --------------------- HTTP FALLBACK ----------------------
void InstallUpdateDialog::startHttpFallback(const QString& sdUrl,
                                            const QString& sdPath,
                                            std::shared_ptr<bool> sdFinished,
                                            std::shared_ptr<bool> sdSuccess,
                                            std::function<void(int, const QString&)> uiProgress,
                                            std::function<void(bool, const QString&)> uiDone)
{
#ifdef _WIN32
    QProcess* ps = new QProcess(this);
    ps->setProcessChannelMode(QProcess::MergedChannels);
    const QString psPath = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));

    // ✅ Script PowerShell avec vitesse fluide et unité adaptée (B/s, KB/s, MB/s)
    const QString script = QString::fromUtf8(
         "$url='%1';$out='%2';"
    "$req=[System.Net.HttpWebRequest]::Create($url);"
    "$req.Proxy=[System.Net.GlobalProxySelection]::GetEmptyWebProxy();"
    "$req.UserAgent='ProjectPlus-Updater';"
    "$res=$req.GetResponse();"
    "$total=$res.ContentLength;"
    "$stream=$res.GetResponseStream();"
    "$fs=[System.IO.FileStream]::new($out,[System.IO.FileMode]::Create);"
    "$buffer=New-Object byte[] (4MB);"  
    "$received=0;$prev=0;"
    "$sw=[System.Diagnostics.Stopwatch]::StartNew();"
    "while(($read=$stream.Read($buffer,0,$buffer.Length)) -gt 0){"
    "  $fs.Write($buffer,0,$read);"
    "  $received+=$read;"
    "  if($sw.ElapsedMilliseconds -gt 500){"
    "    $percent=[math]::Round(($received/$total)*100,1);"
    "    $speedBytes=($received-$prev)/$sw.Elapsed.TotalSeconds;"
    "    if($speedBytes -gt 1048576){$speedStr=[Math]::Round($speedBytes/1MB,2).ToString()+' MB/s'}"
    "    elseif($speedBytes -gt 1024){$speedStr=[Math]::Round($speedBytes/1KB,1).ToString()+' KB/s'}"
    "    else{$speedStr=$speedBytes.ToString()+' B/s'}"
    "    Write-Host (\"$percent|$speedStr\");"
    "    $prev=$received;$sw.Restart()"
    "  }"
    "}"
    "$fs.Close();$stream.Close();$res.Close();"
    "Write-Host 'DONE';"
).arg(sdUrl, sdPath);

    connect(ps, &QProcess::readyReadStandardOutput, this, [this, ps, uiProgress]() {
        const QString out = QString::fromUtf8(ps->readAllStandardOutput()).trimmed();
        if (out.isEmpty())
            return;

        const QStringList lines = out.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
        for (const QString& line : lines)
        {
            const QStringList parts = line.trimmed().split(QLatin1Char('|'));
            if (parts.size() == 2 &&
                parts[0].contains(QRegularExpression(QStringLiteral(R"(^\d+(\.\d+)?)"))))
            {
                const int percent = qBound(0, static_cast<int>(parts[0].toDouble()), 100);
                const QString speed = parts[1].trimmed();
                uiProgress(percent, QStringLiteral("HTTP: %1% (%2)").arg(percent).arg(speed));
            }
            else if (line.contains(QStringLiteral("DONE"), Qt::CaseInsensitive))
            {
                uiProgress(100, QStringLiteral("HTTP: 100% (done)"));
            }
            else
            {
                qDebug().noquote() << "[HTTP]" << line;
            }
        }
    });

    connect(ps, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [=](int exitCode, QProcess::ExitStatus) {
        QFileInfo fi(sdPath);
        const bool ok = (exitCode == 0 && fi.exists() && fi.size() > (10 * 1024 * 1024));
        *sdFinished = true;
        *sdSuccess = ok;

        if (ok)
        {
            uiDone(true, QStringLiteral("✅ SD download complete (HTTPS)!"));
            qDebug().noquote() << "✅ HTTPS download finished successfully.";
        }
        else
        {
            uiDone(false, QStringLiteral("❌ HTTPS download failed."));
            qWarning().noquote() << "❌ HTTPS PowerShell fallback failed.";
            QMessageBox::critical(nullptr,
                                  QStringLiteral("Download failed"),
                                  QStringLiteral("HTTPS download failed.\n\n"
                                                 "Please check your Internet connection or try again later."));
        }

        QMetaObject::invokeMethod(this, [=]() {
            this->checkIfAllDownloadsFinished(*sdFinished, *sdSuccess);
        }, Qt::QueuedConnection);

        ps->deleteLater();
    });

    qDebug().noquote() << "🌐 Launching PowerShell HTTPS fallback...";
    ps->start(psPath.isEmpty() ? QStringLiteral("powershell.exe") : psPath,
              { QStringLiteral("-NoProfile"),
                QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                QStringLiteral("-Command"), script });
#else
    // macOS/Linux fallback (curl)
   // 🐧 macOS/Linux fallback (curl avec vitesse + progression fluide)
QProcess* curl = new QProcess(this);
curl->setProcessChannelMode(QProcess::SeparateChannels);
curl->setReadChannel(QProcess::StandardError);


// 🧠 Variables pour lissage et progression stable
static double curlDisplayed = 0.0;
static double curlTarget    = 0.0;
static int lastPercent = 0;
static QTimer* curlTimer = nullptr;

// ⚙️ Animation fluide (mise à jour toutes les 30 ms)
if (!curlTimer)
{
    curlTimer = new QTimer(this);
    curlTimer->setInterval(30);
    connect(curlTimer, &QTimer::timeout, this, [this]() {
        curlDisplayed += (curlTarget - curlDisplayed) * 0.15;  // interpolation douce
        progressBar->setValue(static_cast<int>(curlDisplayed));
    });
    curlTimer->start();
}



// Progression curl: lire STDERR
connect(curl, &QProcess::readyReadStandardError, this, [this, curl, uiProgress]() {
    static int lastPercent = 0;
    static QElapsedTimer throttle;
    if (!throttle.isValid())
        throttle.start();

    const QString chunk = QString::fromUtf8(curl->readAllStandardError());
    int foundPercent = -1;

    QRegularExpression re(QStringLiteral(R"((\d{1,3}(?:\.\d+)?)%)"));
    auto it = re.globalMatch(chunk);
    while (it.hasNext()) {
        auto m = it.next();
        foundPercent = qBound(0, static_cast<int>(m.captured(1).toDouble()), 100);
    }

    if (foundPercent >= 0 && foundPercent != lastPercent) {
        // anti-reset : ignore un retour à 0 si c’est un glitch visuel
        if (!(foundPercent < lastPercent && (lastPercent - foundPercent) < 90)) {
            // throttle: max 10 updates/sec
            if (throttle.elapsed() > 100) {
                lastPercent = foundPercent;
                uiProgress(lastPercent, QStringLiteral("curl: %1%").arg(lastPercent));
                throttle.restart();
            }
        }
    }
});


connect(curl, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this, [=](int e, QProcess::ExitStatus) {
    QFileInfo fi(sdPath);
    const bool ok = (e == 0 && fi.exists() && fi.size() > (10 * 1024 * 1024));
    *sdFinished = true;
    *sdSuccess = ok;
    uiDone(ok, ok ? QStringLiteral("✅ SD download complete (curl)!")
                  : QStringLiteral("❌ SD download failed (curl)"));

    if (curlTimer) {
        curlTimer->stop();
        curlTimer->deleteLater();
        curlTimer = nullptr;
    }

    if (!ok) {
        QMessageBox::critical(nullptr, QStringLiteral("Download failed"),
                              QStringLiteral("Download via curl also failed."));
    }

    QMetaObject::invokeMethod(this, [=]() {
        this->checkIfAllDownloadsFinished(*sdFinished, *sdSuccess);
    }, Qt::QueuedConnection);
    curl->deleteLater();
});

// 🚀 Lancement du téléchargement
qDebug().noquote() << "🌐 Launching curl:" << sdUrl << "→" << sdPath;

curl->start(QStringLiteral("curl"),
            { QStringLiteral("-L"),             // suit les redirections
              QStringLiteral("--progress-bar"), // affiche la barre sur stderr
              QStringLiteral("--no-buffer"),    // flush immédiat
              QStringLiteral("-o"), sdPath,     // sortie dans sd.raw
              sdUrl });

#endif
}


// --------------------- TIMER ----------------------
void InstallUpdateDialog::timerEvent(QTimerEvent *e)
{
    killTimer(e->timerId());
    if (!downloadUrl.isEmpty())
        download();
    else
        install();
}

// --------------------- INSTALL (simplifié pour l'instant) ----------------------
void InstallUpdateDialog::install()

{
  // ✅ Corrige le dossier temporaire selon la plateforme
QString tmpDir = temporaryDirectory;

#ifdef __APPLE__
// 🧩 macOS : corrige App Translocation (sandbox temporaire)
if (tmpDir.startsWith(QStringLiteral("/private/var/folders/")))
{
    qDebug().noquote() << "⚠️ Detected macOS App Translocation → remapping temporary directory";

    QString appDir = QCoreApplication::applicationDirPath();
    QDir dir(appDir);

    // Remonte jusqu’à trouver portable.txt (point d’ancrage du bundle)
    while (!dir.isRoot() && !QFile::exists(dir.filePath(QStringLiteral("portable.txt"))))
        dir.cdUp();

    if (QFile::exists(dir.filePath(QStringLiteral("portable.txt"))))
    {
        QString baseDir = dir.absolutePath();
        tmpDir = QDir(baseDir).filePath(QStringLiteral("update_tmp"));
        QDir().mkpath(tmpDir);
        qDebug().noquote() << "✅ Using corrected update_tmp path (macOS):" << tmpDir;
    }
}
#else
// 🪟 / 🐧 Windows & Linux : place update_tmp à côté du Dolphin.exe
QString appDir = QCoreApplication::applicationDirPath();
QDir dir(appDir);

// Remonte jusqu’à trouver portable.txt pour les versions portables
while (!dir.isRoot() && !QFile::exists(dir.filePath(QStringLiteral("portable.txt"))))
    dir.cdUp();

QString baseDir = QFile::exists(dir.filePath(QStringLiteral("portable.txt")))
                      ? dir.absolutePath()
                      : QFileInfo(installationDirectory).path();

tmpDir = QDir(baseDir).filePath(QStringLiteral("update_tmp"));
QDir().mkpath(tmpDir);
qDebug().noquote() << "📁 Using update_tmp path (Windows/Linux):" << tmpDir;
#endif

#ifdef _WIN32
// 🧩 Windows — corrige le chemin final du ZIP pour forcer update_tmp à côté de l'exécutable
{
    QString appDir = QCoreApplication::applicationDirPath();
    QDir dir(appDir);

    // Remonte jusqu'à portable.txt si c’est une version portable
    while (!dir.isRoot() && !QFile::exists(dir.filePath(QStringLiteral("portable.txt"))))
        dir.cdUp();

    QString baseDir = QFile::exists(dir.filePath(QStringLiteral("portable.txt")))
                          ? dir.absolutePath()
                          : QFileInfo(installationDirectory).path();

    tmpDir = QDir(baseDir).filePath(QStringLiteral("update_tmp"));
    QDir().mkpath(tmpDir);

    qDebug().noquote() << "📦 [Windows] Forced update_tmp path for ZIP:" << tmpDir;
}
#endif

 QString zipFile = QDir(tmpDir).filePath(filename);

  if (!QFile::exists(zipFile))
        qDebug().noquote() << "ℹ️ ZIP not yet downloaded, waiting for completion...";


   

    if (!QFile::exists(zipFile))
    {
        QMessageBox::critical(this, QStringLiteral("Error"), QStringLiteral("ZIP file missing!"));
        reject();
        return;
    }

    QDir().mkpath(tmpDir);

    label->setText(QStringLiteral("Step 2/2: Installing update..."));
    stepLabel->setText(QStringLiteral("Extracting files..."));
    stepProgressBar->setValue(0);
    progressBar->setValue(75);

    // --- Thread d’extraction ---
    QThread* thread = new QThread(nullptr);

    connect(thread, &QThread::started, this, [this, zipFile, tmpDir, thread]() {
        bool success = unzipFile(zipFile.toStdString(), tmpDir.toStdString(),
            [this](int current, int total)
            {
                const int percent = (total > 0) ? (current * 100 / total) : 0;

                QMetaObject::invokeMethod(QApplication::instance(), [=]() {
                    stepProgressBar->setValue(percent);
                    stepLabel->setText(QStringLiteral("Extracting: %1%").arg(percent));
                    progressBar->setValue(75 + (percent * 0.25));
                }, Qt::QueuedConnection);
            });

        QMetaObject::invokeMethod(QApplication::instance(), [=]() {
            thread->quit();
            thread->deleteLater();

            if (!success)
            {
                QMessageBox::critical(nullptr, QStringLiteral("Error"), QStringLiteral("Failed to extract ZIP file."));
                return;
            }

            QFile::remove(zipFile);
            qDebug().noquote() << QStringLiteral("✅ ZIP extracted to temporary folder:") << tmpDir;

            stepLabel->setText(QStringLiteral("Finalizing update..."));
            stepProgressBar->setValue(100);
            progressBar->setValue(100);


#ifdef __APPLE__
// --------------------------------------------------------------
// 📦 Étapes spécifiques macOS : gestion du .tar et remplacement .app
// --------------------------------------------------------------
QMetaObject::invokeMethod(QApplication::instance(), [=]() {
    QDir tmp(tmpDir);
    QStringList tars = tmp.entryList(QStringList() << QStringLiteral("*.tar"), QDir::Files);
    if (!tars.isEmpty()) {
        QString tarPath = tmp.filePath(tars.first());
        qDebug().noquote() << "📦 Found TAR:" << tarPath;
        QProcess tarProc;
        tarProc.setWorkingDirectory(tmpDir);
        tarProc.start(QStringLiteral("/usr/bin/tar"), {QStringLiteral("-xf"), tarPath});
        tarProc.waitForFinished(60000);
        QFile::remove(tarPath);
        qDebug().noquote() << "✅ TAR extracted and removed.";
    }

    QString newAppPath;
    QStringList apps;
    QDirIterator it(tmpDir, QStringList() << QStringLiteral("*.app"), QDir::Dirs, QDirIterator::Subdirectories);
    while (it.hasNext())
        apps << it.next();

    if (apps.isEmpty()) {
        qWarning().noquote() << "❌ Aucun .app trouvé après extraction TAR";
        return;
    }

    newAppPath = apps.first();
    QString finalAppPath = QDir(tmpDir).filePath(QStringLiteral("ProjectPlusFR.app"));
    if (QFileInfo(newAppPath).fileName() != QStringLiteral("ProjectPlusFR.app")) {
        QDir().rename(newAppPath, finalAppPath);
    }

  QString parentDir = QFileInfo(tmpDir).path();
QString currentBundle = QDir(parentDir).filePath(QStringLiteral("ProjectPlusFR.app"));

// ✅ Crée un script bash robuste pour le déplacement complet
QString script;
script += QStringLiteral("#!/bin/bash\n");
script += QStringLiteral("set -e\n");
script += QStringLiteral("SRC=\"%1\"\n").arg(tmpDir.trimmed());
script += QStringLiteral("DST=\"%1\"\n").arg(currentBundle.trimmed());
script += QStringLiteral("APP_NAME=\"$(basename \"$DST\")\"\n");
script += QStringLiteral("APP_DIR=\"$(dirname \"$DST\")\"\n");
script += QStringLiteral("SCRIPT_PATH=\"$0\"\n");
script += QStringLiteral("echo \"🔍 SRC: $SRC\"\n");
script += QStringLiteral("echo \"🔍 DST: $DST\"\n");
script += QStringLiteral("sleep 2\n");
script += QStringLiteral("echo \"🧹 Removing old app only...\"\n");
script += QStringLiteral("rm -rf \"$DST\"\n");
script += QStringLiteral("echo \"🚚 Moving new version from $SRC to $APP_DIR (replace existing files, keep user data)...\"\n");
script += QStringLiteral("shopt -s dotglob nullglob\n");

// ✅ Déplacement sélectif : tout sauf User/ et fichiers .raw
script += QStringLiteral("for item in \"$SRC\"/*; do\n");
script += QStringLiteral("  name=\"$(basename \"$item\")\"\n");
script += QStringLiteral("  dest=\"$APP_DIR/$name\"\n");
script += QStringLiteral("  if [[ \"$name\" == \"User\" || \"$name\" == *.raw ]]; then\n");
script += QStringLiteral("    echo \"⚠️ Skipping user file: $name\"\n");
script += QStringLiteral("    continue\n");
script += QStringLiteral("  fi\n");
script += QStringLiteral("  echo \"➡️ Updating $name\"\n");
script += QStringLiteral("  rm -rf \"$dest\"\n");
script += QStringLiteral("  mv \"$item\" \"$APP_DIR\"/\n");
script += QStringLiteral("done\n");

script += QStringLiteral("echo \"🧽 Cleaning temporary files...\"\n");
script += QStringLiteral("rm -rf \"$SRC\"\n");
script += QStringLiteral("rm -f \"$APP_DIR/nohup.out\" 2>/dev/null || true\n");  // ✅ supprime nohup.out
script += QStringLiteral("echo \"✅ Relaunching app...\"\n");
script += QStringLiteral("open \"$APP_DIR/$APP_NAME\" || echo \"⚠️ Failed to relaunch app\"\n");
script += QStringLiteral("sleep 2\n");
script += QStringLiteral("echo \"🧹 Cleaning script...\"\n");
script += QStringLiteral("rm -f \"$SCRIPT_PATH\"\n");

// 🧩 Sauvegarde le script temporairement pour éviter les coupures à la fermeture
QString scriptPath = QDir(parentDir).filePath(QStringLiteral("update_relaunch.sh"));
QFile f(scriptPath);
if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
    QTextStream out(&f);
    out << script;
    f.close();
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

qDebug().noquote() << "🚀 Launching independent update script:" << scriptPath;



   // ✅ Ferme proprement la fenêtre avant de quitter
this->close();
QApplication::processEvents();

// ✅ Lancement via nohup (indépendant de Dolphin)
bool started = QProcess::startDetached(QStringLiteral("/usr/bin/nohup"),
                                       {QStringLiteral("/bin/bash"), scriptPath},
                                       tmpDir);

if (!started)
{
    qWarning().noquote() << "❌ Failed to launch relaunch script.";
    QMessageBox::warning(nullptr, QStringLiteral("Update error"),
                         QStringLiteral("Failed to launch update script."));
    return;
}

qDebug().noquote() << "✅ nohup detached successfully, exiting Dolphin...";
QTimer::singleShot(500, [] { ::_exit(0); });

}, Qt::QueuedConnection);

#endif // __APPLE__  ✅ <-- FERMER ici le bloc macOS

// --------------------------------------------------------------
// 🪟 Windows / 🐧 Linux
// --------------------------------------------------------------
#ifdef _WIN32
    // ================================
    // 🪟 WINDOWS : finalisation via script .bat placé dans le dossier cible
    // ================================
    const QString dstDir = QDir::toNativeSeparators(installationDirectory);
    const QString srcDir = QDir::toNativeSeparators(tmpDir);
    const QString exePath = QDir::toNativeSeparators(
        installationDirectory + QDir::separator() + QStringLiteral("Dolphin.exe"));

    // Écrire le .bat dans le dossier destination (PAS dans update_tmp)
    QString batPath = QDir(installationDirectory).filePath(QStringLiteral("update_relaunch.bat"));
    QFile f(batPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QTextStream out(&f);

        out << "@echo off\n";
        out << "setlocal enabledelayedexpansion\n";
        out << "title Project+FR Updater\n";
        out << "set SRC=" << srcDir << "\n";
        out << "set DST=" << dstDir << "\n";
        out << "echo.\n";
        out << "echo ==============================================\n";
        out << "echo 🧩 PROJECT+FR UPDATER - FINAL PHASE\n";
        out << "echo SRC=%SRC%\n";
        out << "echo DST=%DST%\n";
        out << "echo ==============================================\n";
        out << "echo.\n";
        out << "echo Waiting for Dolphin to close...\n";
        out << "timeout /t 2 /nobreak >nul\n";

        // Se placer dans le dossier destination pour éviter tout verrou sur SRC
        out << "cd /d \"%DST%\"\n";

        out << "echo Moving new files to destination...\n";
        out << "robocopy \"%SRC%\" \"%DST%\" /E /MOVE /R:2 /W:1 >nul\n";
        out << "set RC=%ERRORLEVEL%\n";
        out << "if %RC% GEQ 8 (\n";
        out << "  echo ❌ Robocopy failed with code %RC%.\n";
        out << "  pause\n";
        out << "  exit /b %RC%\n";
        out << ")\n";
        out << "echo ✅ Files moved successfully.\n";

         // 🔧 Exécuter le script PowerShell juste après le déplacement
        out << "echo Running backend fix script...\n";
        out << "if exist \"fix_backend.ps1\" (\n";
        out << "  echo ▶ Executing fix_backend.ps1...\n";
        out << "  powershell -NoProfile -ExecutionPolicy Bypass -File \"fix_backend.ps1\"\n";
        out << ") else (\n";
        out << "  echo ⚠️ fix_backend.ps1 not found, skipping backend fix.\n";
        out << ")\n";

        out << "echo Cleaning temporary folder...\n";
        out << "rmdir /s /q \"%SRC%\" >nul 2>&1\n";

        out << "echo Relaunching Dolphin...\n";
        out << "if exist \"Dolphin.exe\" (\n";
        out << "  echo ▶ Launching Dolphin.exe from %CD%\n";
        out << "  start \"Project+FR Relaunch\" \".\\Dolphin.exe\"\n";
        out << ") else (\n";
        out << "  echo ❌ Dolphin.exe not found in %CD%!\n";
        out << "  dir /b\n";
        out << "  pause\n";
        out << ")\n";

        out << "echo Cleaning script...\n";
        out << "del \"%~f0\" >nul 2>&1\n";
        out << "exit\n";

        f.close();
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    qDebug().noquote() << "🚀 Launching Windows update finalizer:" << batPath;

    // 👉 Lancer le .bat avec dossier de travail = installationDirectory (pas update_tmp)
    QString cmdPath = QStringLiteral("cmd.exe");
    QStringList args = {QStringLiteral("/C"), QDir::toNativeSeparators(batPath)};

    qDebug().noquote() << "🧩 Launching .bat via cmd.exe:" << cmdPath << args
                       << "Working dir:" << installationDirectory;

    bool started = QProcess::startDetached(
        cmdPath,
        args,
        QDir::toNativeSeparators(installationDirectory)
    );

    if (!started) {
        qWarning().noquote() << "❌ Failed to start .bat file directly. Trying fallback...";
        // 🔁 Tentative alternative directe sans working dir (certains environnements GitHub CI échouent sinon)
        started = QProcess::startDetached(
            QStringLiteral("cmd.exe"),
            {QStringLiteral("/C"), QDir::toNativeSeparators(batPath)}
        );
    }


    qDebug().noquote() << "✅ Script launched successfully, exiting Dolphin...";
    QTimer::singleShot(500, [] { ::_exit(0); });
#else
    const QString exe = QDir::toNativeSeparators(
        installationDirectory + QDir::separator() + QStringLiteral("Dolphin"));
    QProcess::startDetached(exe, {});
    QCoreApplication::quit();
#endif

        }, Qt::QueuedConnection);
    });
    thread->start();
}












// --------------------- EXTRACTION ZIP ----------------------
bool InstallUpdateDialog::unzipFile(const std::string& zipFilePath,
                                    const std::string& destDir,
                                    std::function<void(int, int)> progressCallback)
{
    qDebug().noquote() << QStringLiteral("📦 Extracting ZIP:") << QString::fromStdString(zipFilePath);

    void* reader = mz_zip_reader_create();
    if (!reader)
        return false;

    if (mz_zip_reader_open_file(reader, zipFilePath.c_str()) != MZ_OK)
    {
        mz_zip_reader_delete(&reader);
        return false;
    }

    // 🔹 Compte total des fichiers dans l’archive
    int total = 0;
    {
        void* tmp = mz_zip_reader_create();
        if (mz_zip_reader_open_file(tmp, zipFilePath.c_str()) == MZ_OK)
        {
            mz_zip_reader_goto_first_entry(tmp);
            while (mz_zip_reader_goto_next_entry(tmp) == MZ_OK)
                total++;
            mz_zip_reader_close(tmp);
            mz_zip_reader_delete(&tmp);
        }
    }

    int current = 0;
    int err = mz_zip_reader_goto_first_entry(reader);

    // ⚙️ Configuration de l’animation fluide de la barre globale (progressBar)
    static double extractDisplayed = 50.0;
    static double extractTarget = 50.0;
    static QTimer* extractTimer = nullptr;

    if (!extractTimer)
    {
        extractTimer = new QTimer(this);
        extractTimer->setInterval(30); // environ 33 FPS
        connect(extractTimer, &QTimer::timeout, this, [this]() {
            // interpolation douce vers la cible
            extractDisplayed += (extractTarget - extractDisplayed) * 0.15;
            progressBar->setValue(static_cast<int>(extractDisplayed));
        });
        extractTimer->start();
    }

    while (err == MZ_OK)
    {
        err = mz_zip_reader_entry_open(reader);
        if (err != MZ_OK)
            break;

        mz_zip_file* info = nullptr;
        mz_zip_reader_entry_get_info(reader, &info);

        if (info && info->filename)
        {
            std::string entry = info->filename;
            std::string outPath = destDir + "/" + entry;

            if (entry.back() == '/')
            {
                QDir().mkpath(QString::fromStdString(outPath));
            }
            else
            {
                QDir().mkpath(QFileInfo(QString::fromStdString(outPath)).path());
                mz_zip_reader_entry_save_file(reader, outPath.c_str());
            }

            current++;
            int percent = (total > 0) ? (current * 100 / total) : 0;

            // 🔹 Barre d’étape (en bas)
            stepProgressBar->setValue(percent);
            stepLabel->setText(QStringLiteral("Extracting: %1%").arg(percent));

            // 🔹 Barre globale (fluide) → 50 → 100 %
            extractTarget = 50.0 + (percent * 0.5);

            // Callback éventuel (si défini ailleurs)
            if (progressCallback)
                progressCallback(current, total);
        }

        mz_zip_reader_entry_close(reader);
        err = mz_zip_reader_goto_next_entry(reader);
    }

    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);

    // 🔚 Termine proprement la progression à 100 %
     if (extractTimer)
    {
        extractTimer->stop();
        extractTimer->deleteLater();
        extractTimer = nullptr;
    }

    progressBar->setValue(100);
    stepProgressBar->setValue(100);
    stepLabel->setText(QStringLiteral("Extraction complete"));

    return true;
}


void InstallUpdateDialog::closeEvent(QCloseEvent* event)
{
    if (m_isClosing)
        return;

    m_isClosing = true;
    qDebug().noquote() << "🛑 User requested to close update dialog.";

    setEnabled(false);

    // Annule tout ce qui pourrait déclencher un invokeMethod(this, …)
    disconnect(this, nullptr, nullptr, nullptr);

    // 🔹 Tuer tous les sous-processus (aria2c, rclone, PowerShell, etc.)
    const auto processes = findChildren<QProcess*>();
    for (QProcess* p : processes)
    {
        if (p && p->state() != QProcess::NotRunning)
        {
            qDebug().noquote() << "🧨 Killing process:" << p->program();
            disconnect(p, nullptr, this, nullptr);
            p->kill();
            if (!p->waitForFinished(1000))
                p->terminate();
        }
    }

    // 🔹 Stoppe proprement les threads
    const auto threads = findChildren<QThread*>();
    for (QThread* t : threads)
    {
        if (t && t->isRunning())
        {
            qDebug().noquote() << "🧵 Stopping thread:" << t;
            disconnect(t, nullptr, this, nullptr);
            t->requestInterruption();
            t->quit();
            t->wait(1000);
        }
    }

    // 🔹 Nettoyage des fichiers temporaires rclone (sd.raw.*)
{
    const QString userWiiPath = QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + QStringLiteral("/User/Wii"));
    QDir dir(userWiiPath);

    QStringList partials = dir.entryList(QStringList() << QStringLiteral("sd.raw.*"), QDir::Files);
    for (const QString& file : partials)
    {
        QString fullPath = dir.filePath(file);
        qDebug().noquote() << "🧹 Removing leftover partial file:" << fullPath;
        QFile::remove(fullPath);
    }
}

    // 🔹 Supprime les timers (pour éviter les callbacks post-fermeture)
    const auto timers = findChildren<QTimer*>();
    for (QTimer* timer : timers)
    {
        if (timer)
        {
            qDebug().noquote() << "⏱️ Deleting timer";
            timer->stop();
            disconnect(timer, nullptr, this, nullptr);
            timer->deleteLater();
        }
    }

    qDebug().noquote() << "✅ Update dialog closed safely.";
    QDialog::closeEvent(event);
}


