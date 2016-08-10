#include "vpngui.h"
#include "config.h"
#include "authdialog.h"

#include <stdexcept>
#include <QApplication>
#include <QIcon>
#include <QSysInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalMapper>
#include <QUuid>
#include <QDnsLookup>
#include <QEventLoop>
#include <QHostAddress>

QStringList _safeResolve(QString hostname, QString nameserver) {
    QDnsLookup dns;
    if (!nameserver.isEmpty()) {
        dns.setNameserver(QHostAddress(nameserver));
    }
    dns.setType(QDnsLookup::A);
    dns.setName(hostname);
    dns.lookup();

    QEventLoop loop;
    QObject::connect(&dns, SIGNAL(finished()), &loop, SLOT(quit()), Qt::QueuedConnection);
    loop.exec();

    if (dns.error() != QDnsLookup::NoError) {
        qDebug() << "DNS lookup failed: " << hostname;
        qDebug() << dns.errorString();
        return QStringList();
    }

    QStringList addresses;
    foreach (QDnsHostAddressRecord record, dns.hostAddressRecords()) {
        qDebug() << "resolved:" << record.value().toString();
        addresses.append(record.value().toString());
    }

    return addresses;
}

QStringList VPNGUI::safeResolve(const QString &hostname) {
    QStringList nameservers;


    // 1. Custom nameserver
    QString appNs(m_appSettings.value("dns_api").toString());
    if (!appNs.isEmpty()) {
        nameservers.append(appNs);
    }

    // 2. System default
    nameservers.append("");

    // 3. Brand nameserver
    QString providerNs(m_providerSettings.value("nameserver").toString());
    if (!providerNs.isEmpty()) {
        nameservers.append(providerNs);
    }

    // 4. Fallback
    nameservers.append("8.8.8.8");
    nameservers.append("8.8.4.4");

    foreach(QString ns, nameservers) {
        QStringList addresses = _safeResolve(hostname, ns);
        if (addresses.size() > 0) {
            return addresses;
        }
    }
    throw std::runtime_error("DNS lookup failed");
}


VPNCreds::~VPNCreds() {
    clear();
}

void VPNCreds::clear() {
    secureStringClear(username);
    secureStringClear(password);
}


VPNGUI::VPNGUI(QObject *parent)
    : QObject(parent)
    , m_connectMapper(NULL)
    , m_trayMenu()
    , m_trayIcon(this)
    , m_gatewaysReply(NULL)
    , m_providerSettings(":/provider.ini", QSettings::IniFormat)
    , m_appSettings(VPNGUI_ORGNAME, getName())
    , m_qnam(this)
    , m_installer(*this)
    , m_openvpn(this, m_installer.getDir().filePath("openvpn.exe"))
    , m_logWindow(NULL)
    , m_settingsWindow(NULL)
{
    m_connectMenu = m_trayMenu.addMenu(tr("Connect"));
    m_disconnectAction = m_trayMenu.addAction(tr("Disconnect"));
    QAction *logAction = m_trayMenu.addAction(tr("View Log"));
    QAction *settingsAction = m_trayMenu.addAction(tr("Settings"));
    QAction *quitAction = m_trayMenu.addAction(tr("Quit"));

    m_disconnectAction->setDisabled(true);

    m_trayIcon.setContextMenu(&m_trayMenu);
    m_trayIcon.setIcon(QIcon(":/icon.png"));

    connect(quitAction, SIGNAL(triggered(bool)), QApplication::instance(), SLOT(quit()));
    connect(logAction, SIGNAL(triggered(bool)), this, SLOT(openLogWindow()));
    connect(settingsAction, SIGNAL(triggered(bool)), this, SLOT(openSettingsWindow()));
    connect(m_disconnectAction, SIGNAL(triggered(bool)), this, SLOT(vpnDisconnect()));

    connect(&m_openvpn, SIGNAL(connected()), this, SLOT(vpnConnected()));
    connect(&m_openvpn, SIGNAL(disconnected()), this, SLOT(vpnDisconnected()));

    if (!m_installer.isInstalled()) {
        m_installer.install();
        m_trayIcon.showMessage(tr("Installed"),
                               tr("%1 is now installed! (version %2)")
                               .arg(getDisplayName(), getFullVersion()));
    }

    // Cleanup OpenVPN config dir
    m_configDir = QDir(m_installer.getDir().filePath("openvpn_config"));
    if (m_configDir.exists()) {
        foreach(QString f, m_configDir.entryList(QDir::Files)) {
            m_configDir.remove(f);
        }
    } else {
        m_configDir.mkdir(".");
    }

    // Show icon *after* installation.
    // When upgrading, we want the user to only see one icon if asked to quit
    // the old version.
    // And if you press Quit while installing it will explode badly.
    m_trayIcon.show();

    // Update gateways list
    queryGateways();
}

VPNGUI::~VPNGUI() {
    m_trayIcon.setVisible(false);
}

QString VPNGUI::getName() const {
    return m_providerSettings.value("name", VPNGUI_NAME).toString();
}

QString VPNGUI::getDisplayName() const {
    return m_providerSettings.value("display_name", VPNGUI_DISPLAY_NAME).toString();
}

QString VPNGUI::getFullVersion() const {
    QString brandedVersion = m_providerSettings.value("version").toString();
    QString fullVersion(VPNGUI_VERSION);
    if (brandedVersion.length() > 0) {
        fullVersion += "-" + brandedVersion;
    }
    return fullVersion;
}

QString VPNGUI::getURL() const {
    return m_providerSettings.value("url", VPNGUI_URL).toString();
}

void VPNGUI::openLogWindow() {
    // Clean up any previous LogWindow
    if (m_logWindow) {
        m_logWindow->close();
        delete m_logWindow;
    }

    m_logWindow = new LogWindow(NULL, *this, m_openvpn);
    connect(&m_openvpn, SIGNAL(logUpdated(QString)), m_logWindow, SLOT(newLogLine(QString)));
    m_logWindow->show();
}

void VPNGUI::openSettingsWindow() {
    // Clean up any previous SettingsWindow
    if (m_settingsWindow) {
        m_settingsWindow->close();
        delete m_settingsWindow;
    }

    m_settingsWindow = new SettingsWindow(NULL, *this, m_appSettings);
    connect(m_settingsWindow, SIGNAL(settingsChanged(QSet<QString>)), this, SLOT(settingsChanged(QSet<QString>)));
    m_settingsWindow->show();
}

void VPNGUI::queryGateways() {
    QNetworkRequest request;

    QString url(m_providerSettings.value("locations_url").toString());

    if (url.isEmpty()) {
        return;
    }

    QString ua(VPNGUI_NAME "/" VPNGUI_VERSION);
    ua += " " + getName() + "/" + getFullVersion();

    request.setUrl(url);
    request.setRawHeader("User-Agent", ua.toUtf8());

    m_gatewaysReply = m_qnam.get(request);
    connect(m_gatewaysReply, &QNetworkReply::finished, this, &VPNGUI::gatewaysQueryFinished);
}

void VPNGUI::updateGatewayList() {
    VPNGateway gw;

    if (m_connectMapper) {
        delete m_connectMapper;
    }
    m_connectMapper = new QSignalMapper(this);

    m_connectMenu->clear();

    foreach (gw, m_gateways) {
        QAction *act = m_connectMenu->addAction(gw.display_name);
        connect(act, SIGNAL(triggered(bool)), m_connectMapper, SLOT(map()));

        m_connectMapper->setMapping(act, gw.hostname);
    }
    connect(m_connectMapper, SIGNAL(mapped(QString)), this, SLOT(vpnConnect(QString)));
}

VPNCreds VPNGUI::handleAuth(bool failed) {
    VPNCreds c;

    if (!failed) {
        // The first time, try stored credentials
        if (readSavedCredentials(c)) {
            return c;
        }
    }

    if (promptCredentials(c)) {
        saveCredentials(c);
        return c;
    }

    // Clicked "Cancel", abort and return empty VPNCreds
    m_openvpn.disconnect();
    c.clear();
    return c;
}

bool VPNGUI::promptCredentials(VPNCreds &c) {
    AuthDialog d(NULL, *this);
    if (d.exec() == QDialog::Rejected) {
        return false;
    }
    c.username = d.getUsername();
    c.password = d.getPassword();
    return true;
}

bool VPNGUI::readSavedCredentials(VPNCreds &c) {
    if (!m_appSettings.contains("auth")) {
        return false;
    }

    QByteArray encrypted = m_appSettings.value("auth").toByteArray();
    if (encrypted.length() < 2) {
        return false;
    }

    PwStore pwstore;
    QString decrypted(pwstore.decrypt(encrypted));

    int sep = decrypted.indexOf(':');
    if (sep == -1) {
        return false;
    }

    c.username = decrypted.left(sep);
    c.password = decrypted.mid(sep + 1);

    return true;
}

void VPNGUI::saveCredentials(const VPNCreds &c) {
    PwStore pwstore;
    QString text(c.username + ":" + c.password);
    QByteArray encrypted(pwstore.encrypt(text));
    m_appSettings.setValue("auth", encrypted);
}


void VPNGUI::vpnConnect(QString hostname) {
    qDebug() << "Connecting to " << hostname;
    m_connectMenu->setDisabled(true);
    m_disconnectAction->setDisabled(false);
    m_openvpn.connect(makeOpenVPNConfig(hostname));
}

void VPNGUI::vpnDisconnect() {
    m_openvpn.disconnect();
}

void VPNGUI::vpnConnected() {
    m_trayIcon.showMessage(tr("Connected"), tr("VPN successfully connected."));
}

void VPNGUI::vpnDisconnected() {
    m_connectMenu->setDisabled(false);
    m_disconnectAction->setDisabled(true);
    m_trayIcon.showMessage(tr("Disconnected"), tr("VPN disconnected."));
}


QString VPNGUI::makeOpenVPNConfig(const QString &hostname) {
    QString name(QUuid::createUuid().toString() + ".ovpn");
    QString path(m_configDir.filePath(name));

    QFile f(path);
    if (!f.open(QFile::WriteOnly | QFile::Text)) {
        qDebug() << "Cannot write config file " << path;
        throw std::runtime_error("Cannot write config file " + path.toStdString());
    }

    QTextStream s(&f);

    // Common
    s << "verb 3\n";
    s << "client\n";
    s << "tls-client\n";
    s << "remote-cert-tls server\n";
    s << "dev tun\n";
    s << "nobind\n";
    s << "persist-key\n";
    s << "persist-tun\n";
    s << "comp-lzo yes\n";
    s << "auth-user-pass\n";
    s << "register-dns\n";
    s << "redirect-gateway def1\n";
    s << "remote-random\n";

    if (m_providerSettings.value("enable_ipv6", true).toBool()
        && m_appSettings.value("ipv6_tunnel", true).toBool()) {
        s << "tun-ipv6\n";
        s << "route-ipv6 2000::/3\n";
    }

    // Ca
    s << "<ca>\n" << m_providerSettings.value("openvpn_ca").toString() << "\n</ca>\n";

    // Remote
    QString remoteParams;
    QString protocol(getCurrentProtocol(m_providerSettings, m_appSettings));
    if (protocol == "udp") {
        remoteParams = "1196 udp";
    } else if (protocol == "udpl") {
        remoteParams = "1194 udp";
    } else if (protocol == "tcp") {
        remoteParams = "443 tcp";
    }

    foreach (QString addr, safeResolve(hostname)) {
        s << "remote " << addr << " " << remoteParams << "\n";
    }

    // Options
    QString httpProxy(m_appSettings.value("http_proxt").toString());
    QString dns(m_appSettings.value("dns_system").toString());

    if (!httpProxy.isEmpty()) {
        s << "http-proxy " << httpProxy << "\n";
    }
    if (!dns.isEmpty()) {
        s << "dhcp-option DNS " << dns << "\n";
    }

    s.flush();
    f.close();
    return path;
}


bool gatewaysSort(const VPNGateway &gw1, const VPNGateway &gw2) {
    return gw1.display_name < gw2.display_name;
}

void VPNGUI::gatewaysQueryFinished() {
    if (!m_gatewaysReply) {
        return;
    }

    if (m_gatewaysReply->error()) {
        m_trayIcon.showMessage(tr("Gateways update error"), m_gatewaysReply->errorString());
        onGUIReady();
        return;
    }

    QString jsonStr = m_gatewaysReply->readAll();
    QJsonDocument json(QJsonDocument::fromJson(jsonStr.toUtf8()));
    QJsonObject root(json.object());
    QJsonArray locations(root["locations"].toArray());

    m_gateways.clear();
    for (int i=0; i<locations.size(); ++i) {
        QJsonObject loc(locations[i].toObject());
        VPNGateway gw;
        gw.display_name = loc["country_name"].toString();
        gw.hostname = loc["hostname"].toString();
        m_gateways.append(gw);
    }

    qSort(m_gateways.begin(), m_gateways.end(), &gatewaysSort);
    updateGatewayList();
    onGUIReady();
}

// Called after queryGateways() has finished/failed
void VPNGUI::onGUIReady() {
    QString autoconnect(m_appSettings.value("autoconnect").toString());
    if (!autoconnect.isEmpty()) {
        vpnConnect(autoconnect);
    }
}

const QSettings &VPNGUI::getBrandingSettings() const {
    return m_providerSettings;
}

const QSettings &VPNGUI::getAppSettings() const {
    return m_appSettings;
}

const QList<VPNGateway> &VPNGUI::getGatewayList() const {
    return m_gateways;
}

// Events that get triggered on settings save
void VPNGUI::settingsChanged(const QSet<QString> &keys) {
    if (keys.contains("start_on_boot")) {
        bool enabled = m_appSettings.value("start_on_boot").toBool();
        if (!m_installer.setStartOnBoot(enabled)) {
            if (enabled) {
                // If we can't enable it, set it back to false.
                m_appSettings.setValue("start_on_boot", false);
            }
        }
    }
}

QString getCurrentProtocol(const QSettings &providerSettings, QSettings &appSettings) {
    QString defaultProtocol(providerSettings.value("default_protocol", "udp").toString());
    QString currentProtocol(appSettings.value("protocol", defaultProtocol).toString());

    QSet<QString> knownProtocols;
    knownProtocols << "udp" << "udpl" << "tcp";

    // Check protocols are supported
    foreach (QString p, knownProtocols) {
        if (!providerSettings.value("protocol_" + p, false).toBool()) {
            knownProtocols.remove(p);
        }
    }

    // Check currentProtocol (to always have an option checked)
    if (!knownProtocols.contains(currentProtocol)) {
        currentProtocol = defaultProtocol;
        appSettings.setValue("protocol", defaultProtocol);
    }

    return currentProtocol;
}
