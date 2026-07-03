#pragma once

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string>
#include <string_view>

/**
 * @brief TLS/SSL 上下文封装，管理 SSL_CTX 的生命周期和配置。
 *
 * 不可拷贝，支持移动语义。配置为服务器模式，要求最低 TLS 1.2，
 * 启用部分写入和移动写缓冲区模式以适配非阻塞 I/O。
 */
class TlsContext {
public:
    /**
     * @brief 构造 TLS 上下文，设置安全策略和密码套件。
     *
     * 配置 SS L_CTX：禁止压缩、禁止重协商时的会话恢复、服务端优先密码套件，
     * 仅使用 HIGH 强度密码，排除匿名/空加密/MD5/PSK/SRP。
     */
    TlsContext() noexcept
        : ctx_(SSL_CTX_new(TLS_server_method()))
    {
        if (!ctx_) return;

        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

        SSL_CTX_set_mode(ctx_,
            SSL_MODE_ENABLE_PARTIAL_WRITE |
            SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

        SSL_CTX_set_options(ctx_,
            SSL_OP_NO_COMPRESSION |
            SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION |
            SSL_OP_CIPHER_SERVER_PREFERENCE);

        SSL_CTX_set_cipher_list(ctx_, "HIGH:!aNULL:!eNULL:!MD5:!PSK:!SRP");
    }

    /// 释放 SSL_CTX 资源
    ~TlsContext() noexcept {
        if (ctx_) SSL_CTX_free(ctx_);
    }

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    /// 移动构造：转移所有权后将源对象置空
    TlsContext(TlsContext&& other) noexcept
        : ctx_(other.ctx_)
    {
        other.ctx_ = nullptr;
    }

    /// 移动赋值：先释放当前资源，再转移源对象的所有权
    TlsContext& operator=(TlsContext&& other) noexcept {
        if (this != &other) {
            if (ctx_) SSL_CTX_free(ctx_);
            ctx_ = other.ctx_;
            other.ctx_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief 加载 PEM 格式的证书和私钥文件。
     * @param cert_file 证书文件路径（PEM 格式）
     * @param key_file  私钥文件路径（PEM 格式）
     * @return 加载成功返回 true，失败（文件不存在/格式错误/密钥不匹配）返回 false
     */
    bool load_certificate(std::string_view cert_file,
                          std::string_view key_file) noexcept
    {
        if (!ctx_) return false;

        if (SSL_CTX_use_certificate_file(ctx_,
                std::string{cert_file}.c_str(), SSL_FILETYPE_PEM) <= 0)
            return false;

        if (SSL_CTX_use_PrivateKey_file(ctx_,
                std::string{key_file}.c_str(), SSL_FILETYPE_PEM) <= 0)
            return false;

        if (!SSL_CTX_check_private_key(ctx_))
            return false;

        return true;
    }

    /**
     * @brief 为此连接创建一个新的 SSL 对象（服务器模式）。
     * @return 新 SSL 对象的指针，失败时返回 nullptr（ctx 为空或内存不足）
     */
    SSL* new_ssl() const noexcept {
        if (!ctx_) return nullptr;
        auto ssl = SSL_new(ctx_);
        if (!ssl) return nullptr;
        SSL_set_accept_state(ssl);
        return ssl;
    }

    /// @return 底层的 SSL_CTX 指针，用于直接调用 OpenSSL API
    SSL_CTX* native() const noexcept { return ctx_; }
    /// @return true 表示 TLS 上下文已成功初始化，可以用于创建连接
    explicit operator bool() const noexcept { return ctx_ != nullptr; }

private:
    SSL_CTX* ctx_ = nullptr;
};
