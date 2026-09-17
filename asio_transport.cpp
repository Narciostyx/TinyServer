#include "asio_transport.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

#include <boost/asio/error.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/strand.hpp>

#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "error.hpp"
#include "log.hpp"
#include "metrics.hpp"

namespace project {

    namespace {

        // 收集 OpenSSL 错误队列，便于定位证书/套件配置问题
        std::string openssl_error()
        {
            std::string out;
            unsigned long e = 0;
            while ((e = ERR_get_error()) != 0) {
                char buf[256] = {};
                ERR_error_string_n(e, buf, sizeof(buf));
                if (!out.empty()) out += "; ";
                out += buf;
            }
            return out.empty() ? std::string("(no OpenSSL error queued)") : out;
        }

        int parse_tls_version(const std::string& v)
        {
            std::string s;
            s.reserve(v.size());
            for (char c : v) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            // 兼容 "1.2" / "tls1.2" / "tlsv1.2"
            const auto ends_with = [&s](const char* suffix) {
                const std::size_t n = std::strlen(suffix);
                return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
            };
            if (ends_with("1.3")) {
#if defined(TLS1_3_VERSION) && OPENSSL_VERSION_NUMBER >= 0x10101000L
                return TLS1_3_VERSION;
#else
                LOG_WARN("TLS1.3 unsupported by this OpenSSL build, fall back to TLS1.2.");
                return TLS1_2_VERSION;
#endif
            }
            if (ends_with("1.1")) return TLS1_1_VERSION;
            if (ends_with("1.0")) return TLS1_VERSION;
            if (ends_with("1.2")) return TLS1_2_VERSION;
            LOG_WARN("Unrecognised Tls_min_version '" + v + "', fall back to TLS1.2.");
            return TLS1_2_VERSION;
        }

        std::string to_lower_trim_dot(std::string s)
        {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            while (!s.empty() && (s.back() == '.' || s.back() == ' '))
                s.pop_back();
            return s;
        }

        std::string trim(const std::string& s)
        {
            std::size_t b = 0, e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
            return s.substr(b, e - b);
        }

        /**
         * 把 "http/1.1,h2" 转成 ALPN 线格式（长度前缀拼接）：
         *   {8,'h','t','t','p','/','1','.','1', 2,'h','2'}
         */
        std::string build_alpn_wire(const std::string& spec)
        {
            std::string wire;
            std::size_t pos = 0;
            while (pos <= spec.size()) {
                const std::size_t comma = spec.find(',', pos);
                const std::string item = trim(spec.substr(
                    pos, comma == std::string::npos ? std::string::npos : comma - pos));
                pos = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;
                if (item.empty()) continue;
                if (item.size() > 255) {
                    LOG_WARN("Tls_alpn: protocol name too long, ignored: " + item);
                    continue;
                }
                wire.push_back(static_cast<char>(item.size()));
                wire += item;
            }
            return wire;
        }

        /**
         * 给证书追加一个静态取值的 X.509 v3 扩展
         * \param x509 目标证书
         * \param nid 扩展类型（如 NID_subject_alt_name）
         * \param value 扩展取值（必须是可写的 char 数组：OpenSSL 该接口签名为 char*）
         * \return true 添加成功
         */
        bool add_x509_ext(X509* x509, int nid, char* value)
        {
            // conf/ctx 传 nullptr：这里只用字面量取值，不需要配置文件或 issuer 上下文
            X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, nullptr, nid, value);
            if (!ext)
                return false;
            const bool ok = (X509_add_ext(x509, ext, -1) == 1);
            X509_EXTENSION_free(ext);
            return ok;
        }

        /**
         * 生成自签证书与私钥（PEM，RSA-2048，含 SAN）。
         *
         * 为什么内置这段：HTTPS 现在默认开启，如果必须先手工执行 openssl 命令才能启动，
         * 默认配置就永远起不来。校验结果前会打 WARN 提示"仅开发用"。
         * 生产环境请把 Tls_auto_self_signed 设为 0 并配置受信任证书。
         *
         * \param cert_path 证书输出路径
         * \param key_path 私钥输出路径（写入后权限收紧为 0600）
         * \param err 失败原因（出参）
         * \return true 表示生成并落盘成功
         */
        bool generate_self_signed_cert(const std::string& cert_path,
                                       const std::string& key_path,
                                       std::string& err)
        {
            EVP_PKEY* pkey = nullptr;
            EVP_PKEY_CTX* kctx = nullptr;
            X509* x509 = nullptr;
            BIO* key_bio = nullptr;
            BIO* cert_bio = nullptr;
            bool ok = false;

            do
            {
                // 1) 生成 RSA-2048 私钥
                kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
                if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0
                    || EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) <= 0
                    || EVP_PKEY_keygen(kctx, &pkey) <= 0)
                {
                    err = "RSA keygen failed: " + openssl_error();
                    break;
                }

                // 2) 组装 X.509 v3 证书（自签：issuer = subject = CN=localhost）
                x509 = X509_new();
                if (!x509)
                {
                    err = "X509_new failed: " + openssl_error();
                    break;
                }
                X509_set_version(x509, 2);   // 2 == X509_VERSION_3
                ASN1_INTEGER_set(X509_get_serialNumber(x509), static_cast<long>(std::time(nullptr)));
                X509_gmtime_adj(X509_getm_notBefore(x509), 0);
                X509_gmtime_adj(X509_getm_notAfter(x509), 60L * 60L * 24L * 365L);   // 有效期 1 年

                X509_NAME* name = X509_get_subject_name(x509);
                if (!name
                    || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) != 1
                    || X509_set_issuer_name(x509, name) != 1
                    || X509_set_pubkey(x509, pkey) != 1)
                {
                    err = "Failed to build the certificate subject/public key: " + openssl_error();
                    break;
                }

                // 3) 扩展：SAN 必须带（否则按主机名校验会直接失败）、basicConstraints、EKU
                char san[] = "DNS:localhost,IP:127.0.0.1,IP:::1";
                char basic[] = "critical,CA:FALSE";
                char eku[] = "serverAuth";
                if (!add_x509_ext(x509, NID_subject_alt_name, san)
                    || !add_x509_ext(x509, NID_basic_constraints, basic)
                    || !add_x509_ext(x509, NID_ext_key_usage, eku))
                {
                    err = "Failed to add X.509 extensions: " + openssl_error();
                    break;
                }

                // 4) 自签
                if (X509_sign(x509, pkey, EVP_sha256()) == 0)
                {
                    err = "X509_sign failed: " + openssl_error();
                    break;
                }

                // 5) 落盘（目录不存在则创建）
                std::error_code ec;
                const std::filesystem::path key_path_fs(key_path);
                if (key_path_fs.has_parent_path())
                    std::filesystem::create_directories(key_path_fs.parent_path(), ec);

                key_bio = BIO_new_file(key_path.c_str(), "w");
                if (!key_bio
                    || PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1)
                {
                    err = "Failed to write private key '" + key_path + "': " + openssl_error();
                    break;
                }
                cert_bio = BIO_new_file(cert_path.c_str(), "w");
                if (!cert_bio || PEM_write_bio_X509(cert_bio, x509) != 1)
                {
                    err = "Failed to write certificate '" + cert_path + "': " + openssl_error();
                    break;
                }
                ok = true;
            } while (false);

            if (key_bio) BIO_free(key_bio);
            if (cert_bio) BIO_free(cert_bio);
            if (x509) X509_free(x509);
            if (pkey) EVP_PKEY_free(pkey);
            if (kctx) EVP_PKEY_CTX_free(kctx);

            if (ok)
            {
                // 私钥权限收紧为 0600
                std::error_code ec;
                std::filesystem::permissions(key_path,
                    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                    std::filesystem::perm_options::replace, ec);
            }
            return ok;
        }

    } // namespace

    bool AsioTransport::CidrRule::matches(const boost::asio::ip::address& addr) const
    {
        if (allow_all) return true;

        std::array<std::uint8_t, 16> bytes{};
        bool v4 = addr.is_v4();
        if (v4) {
            const auto raw = addr.to_v4().to_bytes();
            std::copy(raw.begin(), raw.end(), bytes.begin());
        } else {
            const auto v6 = addr.to_v6();
            if (v6.is_v4_mapped()) {
                // 归一化：::ffff:a.b.c.d 按 IPv4 处理，避免白名单写成两套
                v4 = true;
                const auto raw = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6).to_bytes();
                std::copy(raw.begin(), raw.end(), bytes.begin());
            } else {
                const auto raw = v6.to_bytes();
                std::copy(raw.begin(), raw.end(), bytes.begin());
            }
        }

        if (v4 != is_v4) return false;

        const int width = is_v4 ? 32 : 128;
        const int byte_count = width / 8;
        int remaining = std::min(bits, width);
        for (int i = 0; i < byte_count && remaining > 0; ++i) {
            const int take = std::min(8, remaining);
            const std::uint8_t mask = static_cast<std::uint8_t>(
                take >= 8 ? 0xFFu : ((0xFFu << (8 - take)) & 0xFFu));
            if ((bytes[i] & mask) != (network[i] & mask)) return false;
            remaining -= take;
        }
        return true;
    }

    AsioTransport::AsioTransport(TransportOptions opt, ThreadPool& pool, Handler handler)
        : opt_(std::move(opt)),
          pool_(pool),
          handler_(std::move(handler)),
          state_(std::make_shared<TransportState>()),
          drain_timer_(ioc_),
          signals_(ioc_)
    {
    }

    AsioTransport::~AsioTransport()
    {
        // 先注销活动会话归零回调，避免会话在 ioc_ 析构后回调本对象
        {
            std::lock_guard<std::mutex> lk(life_mu_);
            alive_ = false;
        }
        stop();
        join_threads();
    }

    int AsioTransport::thread_count() const
    {
        if (opt_.io_threads > 0) return opt_.io_threads;
        const unsigned hw = std::thread::hardware_concurrency();
        return static_cast<int>(std::max(2u, hw == 0 ? 2u : hw));
    }

    void AsioTransport::build_options_validated()
    {
        if (!opt_.http.enable && !opt_.https.enable) {
            throw Err("Both HTTP and HTTPS listeners are disabled; nothing to serve.", kErrType::defaultType);
        }
        if (opt_.https.enable) {
            if (!opt_.tls.enable) {
                throw Err("Https_enable=1 requires Tls_enable=1.", kErrType::Tls_init);
            }
            if (opt_.https.port == opt_.http.port && opt_.http.enable) {
                throw Err("HTTP and HTTPS listeners cannot share the same port.", kErrType::defaultType);
            }
        }

        // 解析 IP/CIDR 白名单
        acl_.clear();
        std::string spec = opt_.allow_peers;
        std::size_t pos = 0;
        while (pos <= spec.size()) {
            const std::size_t comma = spec.find(',', pos);
            std::string token = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;

            // trim
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.erase(token.begin());
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.pop_back();
            if (token.empty()) continue;

            CidrRule rule;
            if (token == "*") {
                rule.allow_all = true;
                acl_.push_back(rule);
                continue;
            }

            std::string addr_part = token;
            int bits = -1;
            if (const auto slash = token.find('/'); slash != std::string::npos) {
                addr_part = token.substr(0, slash);
                try {
                    bits = std::stoi(token.substr(slash + 1));
                } catch (...) {
                    throw Err("Invalid CIDR prefix in Allow_peers: " + token, kErrType::defaultType);
                }
            }

            boost::system::error_code ec;
            const auto addr = boost::asio::ip::make_address(addr_part, ec);
            if (ec) {
                throw Err("Invalid IP/CIDR in Allow_peers: " + token + " (" + ec.message() + ")",
                          kErrType::defaultType);
            }

            rule.is_v4 = addr.is_v4();
            const int width = rule.is_v4 ? 32 : 128;
            rule.bits = (bits < 0) ? width : bits;
            if (rule.bits < 0 || rule.bits > width) {
                throw Err("CIDR prefix out of range in Allow_peers: " + token, kErrType::defaultType);
            }
            if (rule.bits == 0) {
                rule.allow_all = true;
                acl_.push_back(rule);
                continue;
            }

            if (rule.is_v4) {
                const auto raw = addr.to_v4().to_bytes();
                std::copy(raw.begin(), raw.end(), rule.network.begin());
            } else {
                const auto raw = addr.to_v6().to_bytes();
                std::copy(raw.begin(), raw.end(), rule.network.begin());
            }
            acl_.push_back(rule);
        }

        if (acl_.empty()) {
            LOG_WARN("Allow_peers is empty: all source addresses are accepted. "
                     "Consider restricting to loopback or an internal subnet.");
        }
    }

    std::shared_ptr<boost::asio::ssl::context>
        AsioTransport::make_tls_context(const std::string& cert, const std::string& key, bool install_sni_cb) const
    {
        auto ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
        ctx->set_options(boost::asio::ssl::context::default_workarounds);
        SSL_CTX* h = ctx->native_handle();

        // 安全策略直接用 OpenSSL 设置：Boost 各版本间 ssl::context::no_sslv2 等枚举的
        // 弃用状态不一致，用 SSL_CTX_set_options 更稳。
        // SSL_OP_NO_SSLv2/NO_SSLv3 在部分 OpenSSL 版本里已被标记为未使用甚至移除，
        // 因此加 #ifdef 保护（真正的下限由 SSL_CTX_set_min_proto_version 保证）。
        long flags = SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE;
#ifdef SSL_OP_NO_SSLv2
        flags |= SSL_OP_NO_SSLv2;
#endif
#ifdef SSL_OP_NO_SSLv3
        flags |= SSL_OP_NO_SSLv3;
#endif
#ifdef SSL_OP_NO_RENEGOTIATION
        flags |= SSL_OP_NO_RENEGOTIATION;
#endif
        if (!opt_.tls.session_tickets) flags |= SSL_OP_NO_TICKET;
        SSL_CTX_set_options(h, flags);

        const int min_version = parse_tls_version(opt_.tls.min_version);
        if (SSL_CTX_set_min_proto_version(h, min_version) != 1) {
            throw Err("SSL_CTX_set_min_proto_version failed: " + openssl_error(), kErrType::Tls_init);
        }

        if (SSL_CTX_use_certificate_chain_file(h, cert.c_str()) != 1) {
            throw Err("Failed to load TLS certificate '" + cert + "': " + openssl_error(), kErrType::Tls_init);
        }
        if (SSL_CTX_use_PrivateKey_file(h, key.c_str(), SSL_FILETYPE_PEM) != 1) {
            throw Err("Failed to load TLS private key '" + key + "': " + openssl_error(), kErrType::Tls_init);
        }
        if (SSL_CTX_check_private_key(h) != 1) {
            throw Err("TLS certificate and private key do not match: " + openssl_error(), kErrType::Tls_init);
        }

        if (!opt_.tls.ciphers.empty() && SSL_CTX_set_cipher_list(h, opt_.tls.ciphers.c_str()) != 1) {
            throw Err("Invalid Tls_ciphers: " + openssl_error(), kErrType::Tls_init);
        }
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
        if (!opt_.tls.ciphersuites.empty() && SSL_CTX_set_ciphersuites(h, opt_.tls.ciphersuites.c_str()) != 1) {
            throw Err("Invalid Tls_ciphersuites: " + openssl_error(), kErrType::Tls_init);
        }
        if (opt_.tls.session_tickets) SSL_CTX_set_num_tickets(h,
            static_cast<std::size_t>(opt_.tls.session_ticket_count < 0 ? 0 : opt_.tls.session_ticket_count));
#endif

        // 会话复用：需要 session id context，否则 OpenSSL 会拒绝复用
        SSL_CTX_set_session_cache_mode(h, SSL_SESS_CACHE_SERVER);
        const std::string sid = opt_.tls.session_id_context.empty()
            ? std::string("TinyServer") : opt_.tls.session_id_context;
        SSL_CTX_set_session_id_context(h,
            reinterpret_cast<const unsigned char*>(sid.data()),
            static_cast<unsigned int>(std::min<std::size_t>(sid.size(), 32)));

        // mTLS：配置了 CA 文件才启用客户端证书校验
        if (!opt_.tls.ca_file.empty()) {
            if (SSL_CTX_load_verify_locations(h, opt_.tls.ca_file.c_str(), nullptr) != 1) {
                throw Err("Failed to load client CA '" + opt_.tls.ca_file + "': " + openssl_error(),
                          kErrType::Tls_init);
            }
            int mode = SSL_VERIFY_PEER;
            if (opt_.tls.require_client_cert) mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
            SSL_CTX_set_verify(h, mode, nullptr);
            SSL_CTX_set_verify_depth(h, opt_.tls.verify_depth > 0 ? opt_.tls.verify_depth : 4);
        }

        // ALPN：协议名列表来自配置 Tls_alpn（默认 http/1.1；上 HTTP/2 时追加 h2）
        if (!alpn_wire_.empty()) {
            SSL_CTX_set_alpn_select_cb(h, &AsioTransport::alpn_select_cb, const_cast<AsioTransport*>(this));
        }

        // SNI：只在默认上下文上挂回调（握手入口永远是默认上下文）
        if (install_sni_cb) {
            SSL_CTX_set_tlsext_servername_callback(h, &AsioTransport::servername_cb);
            SSL_CTX_set_tlsext_servername_arg(h, const_cast<AsioTransport*>(this));
        }

        return ctx;
    }

    int AsioTransport::alpn_select_cb(SSL*, const unsigned char** out, unsigned char* outlen,
                                      const unsigned char* in, unsigned int inlen, void* arg)
    {
        auto* self = static_cast<AsioTransport*>(arg);
        // 协议名列表在 init() 里构建一次，之后只读，因此这里取它的地址是安全的
        const std::string& wire = self->alpn_wire_;
        if (wire.empty()) return SSL_TLSEXT_ERR_NOACK;

        unsigned char* selected = nullptr;
        if (SSL_select_next_proto(&selected, outlen,
                                  reinterpret_cast<const unsigned char*>(wire.data()),
                                  static_cast<unsigned int>(wire.size()),
                                  in, inlen) != OPENSSL_NPN_NEGOTIATED) {
            // 客户端只提供我们不支持的协议（例如只要 h2 而服务端只声明 http/1.1）
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        *out = selected;
        return SSL_TLSEXT_ERR_OK;
    }

    int AsioTransport::servername_cb(SSL* ssl, int*, void* arg)
    {
        auto* self = static_cast<AsioTransport*>(arg);
        const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        if (!name || !*name) return SSL_TLSEXT_ERR_OK;
        if (SSL_CTX* target = self->find_sni_ctx(name))
            SSL_set_SSL_CTX(ssl, target);
        return SSL_TLSEXT_ERR_OK;
    }

    SSL_CTX* AsioTransport::find_sni_ctx(const std::string& host) const
    {
        const std::string key = to_lower_trim_dot(host);
        std::lock_guard<std::mutex> lk(tls_mu_);
        const auto it = sni_ctxs_.find(key);
        return it == sni_ctxs_.end() ? nullptr : it->second->native_handle();
    }

    void AsioTransport::rebuild_tls()
    {
        auto fresh = make_tls_context(opt_.tls.cert_file, opt_.tls.key_file, true);

        std::unordered_map<std::string, std::shared_ptr<boost::asio::ssl::context>> fresh_sni;
        for (const auto& entry : opt_.tls.sni_certs) {
            const std::string host = to_lower_trim_dot(entry.first);
            if (host.empty() || entry.second.first.empty() || entry.second.second.empty()) {
                LOG_WARN("Skip incomplete Tls_sni_certs entry: " + entry.first);
                continue;
            }
            fresh_sni.emplace(host, make_tls_context(entry.second.first, entry.second.second, false));
            LOG_INFO("Registered SNI certificate for host: " + host);
        }

        std::lock_guard<std::mutex> lk(tls_mu_);
        if (tls_ctx_) retired_ctxs_.push_back(tls_ctx_);
        for (auto& kv : sni_ctxs_) retired_ctxs_.push_back(kv.second);
        tls_ctx_ = std::move(fresh);
        sni_ctxs_ = std::move(fresh_sni);
    }

    std::shared_ptr<boost::asio::ssl::context> AsioTransport::tls_ctx_snapshot() const
    {
        std::lock_guard<std::mutex> lk(tls_mu_);
        return tls_ctx_;
    }

    bool AsioTransport::reload_tls()
    {
        if (!opt_.tls.enable) return false;
        try {
            rebuild_tls();
            LOG_INFO("TLS context reloaded; new connections use the refreshed certificate.");
            return true;
        } catch (const std::exception& e) {
            // 保留旧证书继续服务，比"证书写错就崩掉"更安全
            LOG_ERR(std::string("TLS reload failed, keeping previous certificate: ") + e.what());
            return false;
        }
    }

    void AsioTransport::open_listener(Listener& l)
    {
        boost::system::error_code ec;
        const auto addr = boost::asio::ip::make_address(l.opt.address, ec);
        if (ec) {
            throw Err("Invalid listen address '" + l.opt.address + "': " + ec.message(), kErrType::defaultType);
        }
        const boost::asio::ip::tcp::endpoint endpoint(addr, l.opt.port);

        l.acceptor.open(endpoint.protocol(), ec);
        if (ec) {
            throw Err("Failed to open listener socket: " + ec.message(), kErrType::Reactor_init);
        }
        l.acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
        if (addr.is_v6() && opt_.listen_dual_stack) {
            // 双栈：IPv6 监听地址同时接受 IPv4（v4-mapped）连接；失败不影响启动
            boost::system::error_code ignored;
            l.acceptor.set_option(boost::asio::ip::v6_only(false), ignored);
        }
        l.acceptor.bind(endpoint, ec);
        if (ec) {
            throw Err("Bind to " + l.opt.address + ":" + std::to_string(l.opt.port) + " failed: " + ec.message(),
                      kErrType::Listen_init);
        }
        // Listen_backlog = 0 表示使用系统上限（SOMAXCONN）
        const int backlog = opt_.listen_backlog > 0
            ? opt_.listen_backlog
            : boost::asio::socket_base::max_listen_connections;
        l.acceptor.listen(backlog, ec);
        if (ec) {
            throw Err("Listen on " + l.opt.address + ":" + std::to_string(l.opt.port) + " failed: " + ec.message(),
                      kErrType::Listen_init);
        }

        LOG_INFO(std::string("Listening on ") + (l.is_tls ? "https://" : "http://")
                 + l.opt.address + ":" + std::to_string(l.opt.port)
                 + (l.is_tls ? "" : (opt_.redirect_http_to_https ? " (redirect to HTTPS only)" : "")));
    }

    void AsioTransport::init()
    {
        build_options_validated();

        if (opt_.tls.enable) {
            if (opt_.tls.cert_file.empty() || opt_.tls.key_file.empty()) {
                throw Err("Tls_cert_file / Tls_key_file must be set when TLS is enabled.", kErrType::Tls_init);
            }

            // 证书缺失时自动生成自签证书：HTTPS 默认开启，若要求先手工生成证书，
            // 默认配置就永远起不来（生产请把 Tls_auto_self_signed 设为 0）
            {
                std::error_code fs_ec;
                const bool cert_exists = std::filesystem::exists(opt_.tls.cert_file, fs_ec);
                const bool key_exists = std::filesystem::exists(opt_.tls.key_file, fs_ec);
                if (!cert_exists || !key_exists) {
                    if (!opt_.tls.auto_self_signed) {
                        throw Err("TLS certificate/key not found ('" + opt_.tls.cert_file + "', '"
                                  + opt_.tls.key_file + "') and Tls_auto_self_signed=0.",
                                  kErrType::Tls_init);
                    }
                    std::string gen_err;
                    if (!generate_self_signed_cert(opt_.tls.cert_file, opt_.tls.key_file, gen_err)) {
                        throw Err("Failed to auto-generate a self-signed certificate: " + gen_err,
                                  kErrType::Tls_init);
                    }
                    LOG_WARN("已自动生成自签证书（仅用于开发/测试）：" + opt_.tls.cert_file + " 、"
                             + opt_.tls.key_file
                             + "；浏览器/客户端需显式信任或使用 -k。生产请替换为受信任证书并把 Tls_auto_self_signed 设为 0。");
                    // 非回环监听时再提醒一次：自签证书无法被客户端验证，暴露到网络等于可被中间人冒充
                    const std::string& addr = opt_.https.address;
                    if (addr != "127.0.0.1" && addr != "::1" && addr != "localhost") {
                        LOG_WARN("注意：HTTPS 监听地址为 " + addr
                                 + "（非回环），自签证书会暴露到网络，生产环境请务必改用受信任证书。");
                    }
                }
            }

            // ALPN 线格式只在这里构建一次，之后只读（回调参数指向它的缓冲区）
            alpn_wire_ = build_alpn_wire(opt_.tls.alpn);
            rebuild_tls();
        }

        if (opt_.http.enable) {
            auto l = std::make_shared<Listener>(ioc_);
            l->opt = opt_.http;
            l->is_tls = false;
            listeners_.push_back(l);
        }
        if (opt_.https.enable) {
            auto l = std::make_shared<Listener>(ioc_);
            l->opt = opt_.https;
            l->is_tls = true;
            listeners_.push_back(l);
        }

        try {
            for (auto& l : listeners_) open_listener(*l);
        } catch (...) {
            for (auto& l : listeners_) {
                boost::system::error_code ignored;
                l->acceptor.close(ignored);
            }
            listeners_.clear();
            throw;
        }

        state_->on_active_zero = [this]() {
            // 与析构互斥：析构函数会先取 life_mu_ 并置 alive_=false，
            // 因此这里持锁后访问 ioc_ 是安全的
            std::lock_guard<std::mutex> lk(life_mu_);
            if (!alive_) return;
            if (stopping_.load(std::memory_order_relaxed))
                boost::asio::post(ioc_, [this]() { finish_shutdown(); });
        };

        work_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
            boost::asio::make_work_guard(ioc_));

        signals_.add(SIGINT);
        signals_.add(SIGTERM);
        signals_.add(SIGHUP);
        arm_signals();

        for (auto& l : listeners_) do_accept(l);

        LOG_INFO("AsioTransport initialised with " + std::to_string(thread_count()) + " io thread(s).");
    }

    void AsioTransport::arm_signals()
    {
        signals_.async_wait([this](const boost::system::error_code& ec, int signum) { on_signal(ec, signum); });
    }

    void AsioTransport::on_signal(const boost::system::error_code& ec, int signum)
    {
        if (ec) return;   // operation_aborted => 已停机
        if (signum == SIGHUP) {
            LOG_INFO("Received SIGHUP: reloading TLS certificate.");
            reload_tls();
            arm_signals();
            return;
        }
        if (!stopping_.load(std::memory_order_relaxed)) {
            LOG_WARN("Received signal " + std::to_string(signum) + ": graceful shutdown.");
            stop();
            arm_signals();   // 再等一次信号作为"强制退出"
            return;
        }
        LOG_WARN("Received second termination signal: forcing immediate shutdown.");
        // 运维要求"再按一次 Ctrl-C 必须立刻退出"，因此这里跳过排空直接停事件循环
        state_->notify_all(true);
        stop_event_loop();
    }

    void AsioTransport::do_accept(const std::shared_ptr<Listener>& l)
    {
        if (stopping_.load(std::memory_order_relaxed)) return;

        auto sock = std::make_shared<boost::asio::ip::tcp::socket>(boost::asio::make_strand(ioc_));
        l->acceptor.async_accept(*sock,
            [this, l, sock](const boost::system::error_code& ec) {
                if (!ec) {
                    on_accept(l, ec, std::move(*sock));
                } else if (ec == boost::asio::error::operation_aborted) {
                    return;   // acceptor 已关闭 => 停机中，结束 accept 循环
                } else {
                    LOG_WARN("accept failed: " + ec.message());
                    // fd 耗尽时继续循环会变成 100% CPU 的忙等（经典 accept 风暴），
                    // 这里直接停掉该监听器，由运维修正 ulimit / 连接上限后重启
                    if (ec == boost::asio::error::no_descriptors
                        || ec.value() == EMFILE || ec.value() == ENFILE) {
                        LOG_ERR("Accept loop halted because the process ran out of file descriptors.");
                        return;
                    }
                }
                do_accept(l);   // 继续接受（stopping_ 为真时本函数开头即返回）
            });
    }

    bool AsioTransport::peer_allowed(const boost::asio::ip::address& addr) const
    {
        if (acl_.empty()) return true;
        for (const auto& rule : acl_)
            if (rule.matches(addr)) return true;
        return false;
    }

    void AsioTransport::on_accept(const std::shared_ptr<Listener>& l,
                                  const boost::system::error_code& ec,
                                  boost::asio::ip::tcp::socket sock)
    {
        boost::system::error_code ep_ec;
        const auto endpoint = sock.remote_endpoint(ep_ec);
        const std::string peer = ep_ec
            ? std::string("-")
            : (endpoint.address().to_string() + ":" + std::to_string(endpoint.port()));

        if (ec || ep_ec || !peer_allowed(endpoint.address())) {
            LOG_WARN("Blocked connection from " + peer + " (not permitted by Allow_peers).");
            MetricsRegistry::instance().on_connection_rejected();
            boost::system::error_code ignored;
            sock.close(ignored);
            return;
        }

        // 上限判断为软限制：并发 accept 时最多超出 io 线程数条，可接受
        if (state_->active.load(std::memory_order_relaxed) >= opt_.max_connections) {
            LOG_WARN("Max listening connections reached, refuse " + peer);
            MetricsRegistry::instance().on_connection_rejected();
            boost::system::error_code ignored;
            sock.close(ignored);
            return;
        }

        // TCP keepalive：便于回收"对端已消失但未发 FIN"的连接
        if (opt_.tcp_keepalive) {
            boost::system::error_code ignored;
            sock.set_option(boost::asio::socket_base::keep_alive(true), ignored);
        }

        SessionConfig sc;
        sc.is_tls = l->is_tls;
        if (l->is_tls) {
            sc.tls_ctx = tls_ctx_snapshot();
            if (!sc.tls_ctx) {
                LOG_ERR("TLS context unavailable, refuse " + peer);
                boost::system::error_code ignored;
                sock.close(ignored);
                return;
            }
        }
        sc.body_limit = opt_.body_limit;
        sc.header_limit = opt_.header_limit;
        sc.idle_timeout_seconds = opt_.idle_timeout_seconds;
        sc.request_timeout_seconds = opt_.request_timeout_seconds;
        sc.handshake_timeout_seconds = opt_.handshake_timeout_seconds;
        sc.shutdown_timeout_seconds = opt_.shutdown_timeout_seconds;
        sc.redirect_to_https = (!l->is_tls && opt_.redirect_http_to_https);
        sc.https_port = opt_.https.port;
        sc.fallback_host = opt_.http.address;
        sc.peer = peer;
        sc.server_header = opt_.server_header;
        sc.default_content_type = opt_.default_content_type;
        sc.request_id_header = opt_.request_id_header;
        sc.inline_handler = opt_.inline_handler;

        try {
            if (l->is_tls) {
                auto session = std::make_shared<HttpSession<TlsStream>>(
                    std::move(sock), std::move(sc), state_, pool_, handler_);
                session->start();
            } else {
                auto session = std::make_shared<HttpSession<PlainStream>>(
                    std::move(sock), std::move(sc), state_, pool_, handler_);
                session->start();
            }
        } catch (const std::exception& e) {
            LOG_ERR(std::string("Failed to create session for ") + peer + ": " + e.what());
            boost::system::error_code ignored;
            sock.close(ignored);
        }
    }

    void AsioTransport::start_threads()
    {
        const int total = thread_count();
        threads_.reserve(static_cast<std::size_t>(std::max(0, total - 1)));
        for (int i = 1; i < total; ++i)
            threads_.emplace_back([this]() { ioc_.run(); });
    }

    void AsioTransport::run()
    {
        ioc_.run();
    }

    void AsioTransport::join_threads()
    {
        for (auto& t : threads_)
            if (t.joinable()) t.join();
        threads_.clear();
    }

    void AsioTransport::stop()
    {
        bool expected = false;
        if (!stopping_.compare_exchange_strong(expected, true))
            return;   // 幂等
        boost::asio::post(ioc_, [this]() { begin_shutdown(); });
    }

    void AsioTransport::begin_shutdown()
    {
        state_->stopping.store(true, std::memory_order_relaxed);
        MetricsRegistry::instance().set_readiness(false);
        LOG_WARN("AsioTransport: shutting down, no longer accepting connections.");

        for (auto& l : listeners_) {
            boost::system::error_code ec;
            l->acceptor.close(ec);
        }

        // 通知所有会话：空闲的连接立即关闭，正在处理请求的写完响应再关闭
        state_->notify_all(false);

        if (state_->active.load(std::memory_order_acquire) == 0) {
            finish_shutdown();
            return;
        }

        const int grace = opt_.shutdown_timeout_seconds > 0 ? opt_.shutdown_timeout_seconds : 1;
        drain_timer_.expires_after(std::chrono::seconds(grace));
        drain_timer_.async_wait([this, grace](const boost::system::error_code& ec) {
            if (ec) return;
            LOG_WARN("AsioTransport: drain timeout (" + std::to_string(grace)
                     + "s) reached with "
                     + std::to_string(state_->active.load(std::memory_order_relaxed))
                     + " connection(s) still active, forcing shutdown.");
            finish_shutdown();
        });
    }

    /**
     * 收尾分两阶段，目的是保证"所有会话的 socket 都在 io_context 仍然存活时关闭"：
     *   阶段一：如果没有存活会话，直接停事件循环；
     *           否则发出硬停机通知（会话立即关闭 socket、取消定时器），
     *           再给 500ms 让这些关闭回调在本就存活的事件循环上跑完；
     *   阶段二：重置 work guard 并 stop，run() 随即返回。
     * 若不这样做，残留会话会在 io_context 析构时被连带销毁，
     * 其 socket 析构会访问已经析构的 Asio 服务，属于未定义行为。
     */
    void AsioTransport::finish_shutdown()
    {
        bool expected = false;
        if (!finished_.compare_exchange_strong(expected, true))
            return;   // 幂等：可能同时由 on_active_zero 与 drain 超时触发

        // 注意：新版 Boost.Asio 移除了 cancel(ec) / expires_after(d, ec) 重载，
        // 这里改用无参 cancel()（自身不抛异常）。
        drain_timer_.cancel();

        if (state_->active.load(std::memory_order_acquire) == 0) {
            stop_event_loop();
            return;
        }

        state_->notify_all(true);   // 第二轮：硬关，不再等待响应
        drain_timer_.expires_after(std::chrono::milliseconds(
            opt_.shutdown_drain_grace_ms > 0 ? opt_.shutdown_drain_grace_ms : 500));
        drain_timer_.async_wait([this](const boost::system::error_code& e) {
            if (e) return;
            stop_event_loop();
        });
    }

    void AsioTransport::stop_event_loop()
    {
        boost::system::error_code ec;
        signals_.cancel(ec);        // signal_set 的 async_wait 属于"未完成工作"，会一直占住 io_context
        if (work_) work_->reset();
        LOG_WARN("AsioTransport: event loop stopping ("
                 + std::to_string(state_->active.load(std::memory_order_relaxed))
                 + " connection(s) still tracked).");
        ioc_.stop();
    }

} // namespace project
