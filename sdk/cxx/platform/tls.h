#ifndef ASTERNET_TLS_H
#define ASTERNET_TLS_H

#include <openssl/ssl.h>

#include <string>

namespace asternet {
namespace platform {

bool load_ca_bundle(SSL_CTX *context, const std::string &pem_bundle);
bool verify_certificate_chain(const std::string &pem_bundle, const std::string &host,
                              const unsigned char *certs[], const size_t cert_lengths[],
                              size_t cert_count);

}  // namespace platform
}  // namespace asternet

#endif  // ASTERNET_TLS_H
