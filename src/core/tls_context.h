#pragma once

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string>
#include <string_view>

class TlsContext {
public:
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

    ~TlsContext() noexcept {
        if (ctx_) SSL_CTX_free(ctx_);
    }

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    TlsContext(TlsContext&& other) noexcept
        : ctx_(other.ctx_)
    {
        other.ctx_ = nullptr;
    }

    TlsContext& operator=(TlsContext&& other) noexcept {
        if (this != &other) {
            if (ctx_) SSL_CTX_free(ctx_);
            ctx_ = other.ctx_;
            other.ctx_ = nullptr;
        }
        return *this;
    }

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

    SSL* new_ssl() const noexcept {
        if (!ctx_) return nullptr;
        auto ssl = SSL_new(ctx_);
        if (!ssl) return nullptr;
        SSL_set_accept_state(ssl);
        return ssl;
    }

    SSL_CTX* native() const noexcept { return ctx_; }
    explicit operator bool() const noexcept { return ctx_ != nullptr; }

private:
    SSL_CTX* ctx_ = nullptr;
};
