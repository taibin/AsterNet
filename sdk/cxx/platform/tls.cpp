#include "platform/tls.h"

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

namespace asternet {
namespace platform {

bool load_ca_bundle(SSL_CTX *context, const std::string &pem_bundle) {
    if (context == nullptr || pem_bundle.empty()) return false;
    BIO *bio = BIO_new_mem_buf(pem_bundle.data(), static_cast<int>(pem_bundle.size()));
    if (bio == nullptr) return false;

    X509_STORE *store = SSL_CTX_get_cert_store(context);
    size_t loaded = 0;
    for (;;) {
        X509 *certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (certificate == nullptr) break;
        if (X509_STORE_add_cert(store, certificate) == 1) ++loaded;
        X509_free(certificate);
    }
    BIO_free(bio);
    return loaded > 0;
}

bool verify_certificate_chain(const std::string &pem_bundle, const std::string &host,
                              const unsigned char *certs[], const size_t cert_lengths[],
                              size_t cert_count) {
    if (host.empty() || certs == nullptr || cert_lengths == nullptr
        || cert_count == 0) return false;
    SSL_CTX *context = SSL_CTX_new(TLS_client_method());
    if (context == nullptr || (!pem_bundle.empty() && !load_ca_bundle(context, pem_bundle))) {
        if (context != nullptr) SSL_CTX_free(context);
        return false;
    }

    X509_STORE *store = SSL_CTX_get_cert_store(context);
    if (pem_bundle.empty() && X509_STORE_set_default_paths(store) != 1) {
        SSL_CTX_free(context);
        return false;
    }

    X509 *leaf = nullptr;
    STACK_OF(X509) *chain = sk_X509_new_null();
    bool valid = chain != nullptr;
    for (size_t i = 0; valid && i < cert_count; ++i) {
        const unsigned char *data = certs[i];
        X509 *certificate = d2i_X509(nullptr, &data, static_cast<long>(cert_lengths[i]));
        if (certificate == nullptr) {
            valid = false;
        } else if (i == 0) {
            leaf = certificate;
        } else if (sk_X509_push(chain, certificate) == 0) {
            X509_free(certificate);
            valid = false;
        }
    }

    X509_STORE_CTX *store_context = valid ? X509_STORE_CTX_new() : nullptr;
    if (store_context == nullptr || leaf == nullptr
        || X509_STORE_CTX_init(store_context, store, leaf, chain) != 1) {
        valid = false;
    }
    if (valid) {
        X509_VERIFY_PARAM *parameters = X509_STORE_CTX_get0_param(store_context);
        valid = X509_VERIFY_PARAM_set_purpose(parameters, X509_PURPOSE_SSL_SERVER) == 1
            && X509_VERIFY_PARAM_set1_host(parameters, host.c_str(), host.size()) == 1
            && X509_verify_cert(store_context) == 1;
    }

    if (store_context != nullptr) X509_STORE_CTX_free(store_context);
    if (chain != nullptr) sk_X509_pop_free(chain, X509_free);
    if (leaf != nullptr) X509_free(leaf);
    SSL_CTX_free(context);
    return valid;
}

}  // namespace platform
}  // namespace asternet
