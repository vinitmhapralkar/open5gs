/*
 * Copyright (C) 2019-2023 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ogs-sbi.h"

int __ogs_sbi_domain;
static ogs_sbi_context_t self;
static int context_initialized = 0;

static OGS_POOL(nf_instance_pool, ogs_sbi_nf_instance_t);
static OGS_POOL(nf_service_pool, ogs_sbi_nf_service_t);
static OGS_POOL(xact_pool, ogs_sbi_xact_t);
static OGS_POOL(subscription_spec_pool, ogs_sbi_subscription_spec_t);
static OGS_POOL(subscription_data_pool, ogs_sbi_subscription_data_t);
static OGS_POOL(smf_info_pool, ogs_sbi_smf_info_t);
static OGS_POOL(nf_info_pool, ogs_sbi_nf_info_t);

void ogs_sbi_context_init(OpenAPI_nf_type_e nf_type)
{
    char nf_instance_id[OGS_UUID_FORMATTED_LENGTH + 1];

    ogs_assert(nf_type);

    ogs_assert(context_initialized == 0);

    /* Initialize SBI context */
    memset(&self, 0, sizeof(ogs_sbi_context_t));

    ogs_log_install_domain(&__ogs_sbi_domain, "sbi", ogs_core()->log.level);

    ogs_sbi_message_init(ogs_app()->pool.message, ogs_app()->pool.message);
    ogs_sbi_server_init(ogs_app()->pool.event, ogs_app()->pool.event);
    ogs_sbi_client_init(ogs_app()->pool.event, ogs_app()->pool.event);

    ogs_list_init(&self.nf_instance_list);
    ogs_pool_init(&nf_instance_pool, ogs_app()->pool.nf);
    ogs_pool_init(&nf_service_pool, ogs_app()->pool.nf_service);

    ogs_pool_init(&xact_pool, ogs_app()->pool.xact);

    ogs_list_init(&self.subscription_spec_list);
    ogs_pool_init(&subscription_spec_pool, ogs_app()->pool.subscription);

    ogs_list_init(&self.subscription_data_list);
    ogs_pool_init(&subscription_data_pool, ogs_app()->pool.subscription);

    ogs_pool_init(&smf_info_pool, ogs_app()->pool.nf);

    ogs_pool_init(&nf_info_pool, ogs_app()->pool.nf * OGS_MAX_NUM_OF_NF_INFO);

    /* Add SELF NF-Instance */
    self.nf_instance = ogs_sbi_nf_instance_add();
    ogs_assert(self.nf_instance);

    ogs_uuid_get(&self.uuid);
    ogs_uuid_format(nf_instance_id, &self.uuid);
    ogs_sbi_nf_instance_set_id(self.nf_instance, nf_instance_id);
    ogs_sbi_nf_instance_set_type(self.nf_instance, nf_type);

    /* Add NRF NF-Instance */
    if (nf_type != OpenAPI_nf_type_NRF) {
        self.nrf_instance = ogs_sbi_nf_instance_add();
        ogs_assert(self.nrf_instance);
        ogs_sbi_nf_instance_set_type(self.nrf_instance, OpenAPI_nf_type_NRF);
    }

    /* Add SCP NF-Instance */
    if (nf_type != OpenAPI_nf_type_NRF) {
        self.scp_instance = ogs_sbi_nf_instance_add();
        ogs_assert(self.scp_instance);
        ogs_sbi_nf_instance_set_type(self.scp_instance, OpenAPI_nf_type_SCP);
    }

    context_initialized = 1;
}

void ogs_sbi_context_final(void)
{
    ogs_assert(context_initialized == 1);

    ogs_sbi_subscription_data_remove_all();
    ogs_pool_final(&subscription_data_pool);

    ogs_sbi_subscription_spec_remove_all();
    ogs_pool_final(&subscription_spec_pool);

    ogs_pool_final(&xact_pool);

    ogs_sbi_nf_instance_remove_all();

    ogs_pool_final(&nf_instance_pool);
    ogs_pool_final(&nf_service_pool);
    ogs_pool_final(&smf_info_pool);

    ogs_pool_final(&nf_info_pool);

    ogs_sbi_client_final();
    ogs_sbi_server_final();
    ogs_sbi_message_final();

    context_initialized = 0;
}

ogs_sbi_context_t *ogs_sbi_self(void)
{
    return &self;
}

static int ogs_sbi_context_prepare(void)
{
#if ENABLE_ACCEPT_ENCODING
    self.content_encoding = "gzip";
#endif

    self.tls.server.scheme = OpenAPI_uri_scheme_http;
    self.tls.client.scheme = OpenAPI_uri_scheme_http;

    return OGS_OK;
}

static int ogs_sbi_context_validation(
        const char *local, const char *nrf, const char *scp)
{
    /* If SMF is only used in 4G EPC, no SBI interface is required.  */
    if (local && strcmp(local, "smf") != 0 &&
        ogs_list_first(&self.server_list) == NULL) {
        ogs_error("No %s.sbi.address: in '%s'", local, ogs_app()->file);
        return OGS_ERROR;
    }

    ogs_assert(context_initialized == 1);
    switch (self.discovery_config.delegated) {
    case OGS_SBI_DISCOVERY_DELEGATED_AUTO:
        if (local && strcmp(local, "nrf") == 0) {
            /* Skip NRF */
        } else if (local && strcmp(local, "scp") == 0) {
            /* Skip SCP */
        } else if (local && strcmp(local, "smf") == 0) {
            /* Skip SMF since SMF can run 4G */
        } else {
            if (NF_INSTANCE_CLIENT(self.nrf_instance) ||
                NF_INSTANCE_CLIENT(self.scp_instance)) {
            } else {
                ogs_error("DELEGATED_AUTO - Both NRF and %s are unavailable",
                        scp && strcmp(scp, "next_scp") == 0 ?
                            "Next-hop SCP" : "SCP");
                return OGS_ERROR;
            }
        }
        break;
    case OGS_SBI_DISCOVERY_DELEGATED_YES:
        if (NF_INSTANCE_CLIENT(self.scp_instance) == NULL) {
            ogs_error("DELEGATED_YES - no %s available",
                    scp && strcmp(scp, "next_scp") == 0 ?
                        "Next-hop SCP" : "SCP");
            return OGS_ERROR;
        }
        break;
    case OGS_SBI_DISCOVERY_DELEGATED_NO:
        if (NF_INSTANCE_CLIENT(self.nrf_instance) == NULL) {
            ogs_error("DELEGATED_NO - no NRF available");
            return OGS_ERROR;
        }
        break;
    default:
        ogs_fatal("Invalid dicovery-config delegated [%d]",
                    self.discovery_config.delegated);
        ogs_assert_if_reached();
    }

    if (ogs_sbi_self()->tls.server.scheme == OpenAPI_uri_scheme_https) {
        if (!ogs_sbi_self()->tls.server.private_key) {
            ogs_error("HTTPS scheme enabled but no server key");
            return OGS_ERROR;
        }
        if (!ogs_sbi_self()->tls.server.cert) {
            ogs_error("HTTPS scheme enabled but no server certificate");
            return OGS_ERROR;
        }
    }

    if (ogs_sbi_self()->tls.server.verify_client) {
        if (!ogs_sbi_self()->tls.server.verify_client_cacert) {
            ogs_error("CLIENT verification enabled but no CA certificate");
            return OGS_ERROR;
        }
    }

    return OGS_OK;
}

int ogs_sbi_context_parse_config(
        const char *local, const char *nrf, const char *scp)
{
    int rv;
    yaml_document_t *document = NULL;
    ogs_yaml_iter_t root_iter;

    document = ogs_app()->document;
    ogs_assert(document);

    rv = ogs_sbi_context_prepare();
    if (rv != OGS_OK) return rv;

    ogs_yaml_iter_init(&root_iter, document);
    while (ogs_yaml_iter_next(&root_iter)) {
        const char *root_key = ogs_yaml_iter_key(&root_iter);
        ogs_assert(root_key);
        if (local && !strcmp(root_key, local)) {
            ogs_yaml_iter_t local_iter;
            ogs_yaml_iter_recurse(&root_iter, &local_iter);
            while (ogs_yaml_iter_next(&local_iter)) {
                const char *local_key = ogs_yaml_iter_key(&local_iter);
                ogs_assert(local_key);
                if (!strcmp(local_key, "defconfig")) {
                    ogs_yaml_iter_t defconfig_iter;
                    ogs_yaml_iter_recurse(&local_iter, &defconfig_iter);
                    while (ogs_yaml_iter_next(&defconfig_iter)) {
                        const char *defconfig_key =
                            ogs_yaml_iter_key(&defconfig_iter);
                        ogs_assert(defconfig_key);
                        if (!strcmp(defconfig_key, "tls")) {
                            ogs_yaml_iter_t tls_iter;
                            ogs_yaml_iter_recurse(&defconfig_iter, &tls_iter);
                            while (ogs_yaml_iter_next(&tls_iter)) {
                                const char *tls_key =
                                    ogs_yaml_iter_key(&tls_iter);
                                ogs_assert(tls_key);
                                if (!strcmp(tls_key, "server")) {
                                    ogs_yaml_iter_t server_iter;
                                    ogs_yaml_iter_recurse(
                                            &tls_iter, &server_iter);
                                    while (ogs_yaml_iter_next(&server_iter)) {
                                        const char *server_key =
                                            ogs_yaml_iter_key(&server_iter);
                                        ogs_assert(server_key);
                                        if (!strcmp(server_key, "scheme")) {
                                            const char *v = ogs_yaml_iter_value(
                                                    &server_iter);
                                            if (v) {
                                                if (!ogs_strcasecmp(
                                                            v, "https"))
                                                    self.tls.server.scheme =
                                                    OpenAPI_uri_scheme_https;
                                                else if (!ogs_strcasecmp(
                                                            v, "http"))
                                                    self.tls.server.scheme =
                                                    OpenAPI_uri_scheme_http;
                                                else
                                                    ogs_warn(
                                                        "unknown scheme `%s`",
                                                        v);
                                            }
                                        } else if (!strcmp(server_key,
                                                    "private_key")) {
                                            self.tls.server.private_key =
                                                ogs_yaml_iter_value(
                                                        &server_iter);
                                        } else if (!strcmp(server_key,
                                                    "cert")) {
                                            self.tls.server.cert =
                                                ogs_yaml_iter_value(
                                                        &server_iter);
                                        } else if (!strcmp(server_key,
                                                    "verify_client")) {
                                            self.tls.server.verify_client =
                                                ogs_yaml_iter_bool(
                                                        &server_iter);
                                        } else if (!strcmp(server_key,
                                                    "verify_client_cacert")) {
                                            self.tls.server.
                                                verify_client_cacert =
                                                    ogs_yaml_iter_value(
                                                        &server_iter);
                                        }
                                    }
                                } else if (!strcmp(tls_key, "client")) {
                                    ogs_yaml_iter_t client_iter;
                                    ogs_yaml_iter_recurse(
                                            &tls_iter, &client_iter);
                                    while (ogs_yaml_iter_next(&client_iter)) {
                                        const char *client_key =
                                            ogs_yaml_iter_key(&client_iter);
                                        ogs_assert(client_key);
                                        if (!strcmp(client_key, "scheme")) {
                                            const char *v = ogs_yaml_iter_value(
                                                    &client_iter);
                                            if (v) {
                                                if (!ogs_strcasecmp(
                                                            v, "https"))
                                                    self.tls.client.scheme =
                                                    OpenAPI_uri_scheme_https;
                                                else if (!ogs_strcasecmp(
                                                            v, "http"))
                                                    self.tls.client.scheme =
                                                    OpenAPI_uri_scheme_http;
                                                else
                                                    ogs_warn(
                                                        "unknown scheme `%s`",
                                                        v);
                                            }
                                        } else if (!strcmp(client_key,
                                                    "insecure_skip_verify")) {
                                            self.tls.client.
                                                insecure_skip_verify =
                                                    ogs_yaml_iter_bool(
                                                        &client_iter);
                                        } else if (!strcmp(client_key,
                                                    "cacert")) {
                                            self.tls.client.cacert =
                                                ogs_yaml_iter_value(
                                                        &client_iter);
                                        } else if (!strcmp(client_key,
                                                    "client_private_key")) {
                                            self.tls.client.private_key =
                                                ogs_yaml_iter_value(
                                                        &client_iter);
                                        } else if (!strcmp(client_key,
                                                    "client_cert")) {
                                            self.tls.client.cert =
                                                ogs_yaml_iter_value(
                                                        &client_iter);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ogs_yaml_iter_init(&root_iter, document);
    while (ogs_yaml_iter_next(&root_iter)) {
        const char *root_key = ogs_yaml_iter_key(&root_iter);
        ogs_assert(root_key);
        if (local && !strcmp(root_key, local)) {
            ogs_yaml_iter_t local_iter;
            ogs_yaml_iter_recurse(&root_iter, &local_iter);
            while (ogs_yaml_iter_next(&local_iter)) {
                const char *local_key = ogs_yaml_iter_key(&local_iter);
                ogs_assert(local_key);
                if (!strcmp(local_key, "sbi")) {
                    ogs_sbi_server_t *server = NULL;
                    OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;
                    ogs_list_t list, list6;
                    ogs_socknode_t *node = NULL, *node6 = NULL;

                    ogs_yaml_iter_t sbi_array, sbi_iter;
                    ogs_yaml_iter_recurse(&local_iter, &sbi_array);
                    do {
                        int i, family = AF_UNSPEC;
                        int num = 0;
                        const char *hostname[OGS_MAX_NUM_OF_HOSTNAME];
                        int num_of_advertise = 0;
                        const char *advertise[OGS_MAX_NUM_OF_HOSTNAME];

                        uint16_t port = 0;
                        const char *dev = NULL;
                        ogs_sockaddr_t *addr = NULL;

                        const char *private_key = NULL, *cert = NULL;

                        bool verify_client = false;
                        const char *verify_client_cacert = NULL;

                        ogs_sockopt_t option;
                        bool is_option = false;

                        if (ogs_yaml_iter_type(&sbi_array) ==
                                YAML_MAPPING_NODE) {
                            memcpy(&sbi_iter, &sbi_array,
                                    sizeof(ogs_yaml_iter_t));
                        } else if (ogs_yaml_iter_type(&sbi_array) ==
                            YAML_SEQUENCE_NODE) {
                            if (!ogs_yaml_iter_next(&sbi_array))
                                break;
                            ogs_yaml_iter_recurse(&sbi_array, &sbi_iter);
                        } else if (ogs_yaml_iter_type(&sbi_array) ==
                            YAML_SCALAR_NODE) {
                            break;
                        } else
                            ogs_assert_if_reached();

                        while (ogs_yaml_iter_next(&sbi_iter)) {
                            const char *sbi_key =
                                ogs_yaml_iter_key(&sbi_iter);
                            ogs_assert(sbi_key);
                            if (!strcmp(sbi_key, "family")) {
                                const char *v = ogs_yaml_iter_value(&sbi_iter);
                                if (v) family = atoi(v);
                                if (family != AF_UNSPEC &&
                                    family != AF_INET && family != AF_INET6) {
                                    ogs_warn("Ignore family(%d) : "
                                        "AF_UNSPEC(%d), "
                                        "AF_INET(%d), AF_INET6(%d) ",
                                        family, AF_UNSPEC, AF_INET, AF_INET6);
                                    family = AF_UNSPEC;
                                }
                            } else if (!strcmp(sbi_key, "address")) {
                                ogs_yaml_iter_t hostname_iter;
                                ogs_yaml_iter_recurse(&sbi_iter,
                                        &hostname_iter);
                                ogs_assert(ogs_yaml_iter_type(&hostname_iter) !=
                                    YAML_MAPPING_NODE);

                                do {
                                    if (ogs_yaml_iter_type(&hostname_iter) ==
                                            YAML_SEQUENCE_NODE) {
                                        if (!ogs_yaml_iter_next(
                                                    &hostname_iter))
                                            break;
                                    }

                                    ogs_assert(num < OGS_MAX_NUM_OF_HOSTNAME);
                                    hostname[num++] =
                                        ogs_yaml_iter_value(&hostname_iter);
                                } while (
                                    ogs_yaml_iter_type(&hostname_iter) ==
                                        YAML_SEQUENCE_NODE);
                            } else if (!strcmp(sbi_key, "advertise")) {
                                ogs_yaml_iter_t advertise_iter;
                                ogs_yaml_iter_recurse(&sbi_iter,
                                        &advertise_iter);
                                ogs_assert(ogs_yaml_iter_type(
                                    &advertise_iter) != YAML_MAPPING_NODE);

                                do {
                                    if (ogs_yaml_iter_type(&advertise_iter) ==
                                                YAML_SEQUENCE_NODE) {
                                        if (!ogs_yaml_iter_next(
                                                    &advertise_iter))
                                            break;
                                    }

                                    ogs_assert(num_of_advertise <
                                            OGS_MAX_NUM_OF_HOSTNAME);
                                    advertise[num_of_advertise++] =
                                        ogs_yaml_iter_value(&advertise_iter);
                                } while (
                                    ogs_yaml_iter_type(&advertise_iter) ==
                                        YAML_SEQUENCE_NODE);
                            } else if (!strcmp(sbi_key, "port")) {
                                const char *v = ogs_yaml_iter_value(&sbi_iter);
                                if (v)
                                    port = atoi(v);
                            } else if (!strcmp(sbi_key, "dev")) {
                                dev = ogs_yaml_iter_value(&sbi_iter);
                            } else if (!strcmp(sbi_key, "scheme")) {
                                const char *v = ogs_yaml_iter_value(&sbi_iter);
                                if (v) {
                                    if (!ogs_strcasecmp(v, "https"))
                                        scheme = OpenAPI_uri_scheme_https;
                                    else if (!ogs_strcasecmp(v, "http"))
                                        scheme = OpenAPI_uri_scheme_http;
                                    else
                                        ogs_warn("unknown scheme `%s`", v);
                                }
                            } else if (!strcmp(sbi_key, "private_key")) {
                                private_key = ogs_yaml_iter_value(&sbi_iter);
                            } else if (!strcmp(sbi_key, "cert")) {
                                cert = ogs_yaml_iter_value(&sbi_iter);
                            } else if (!strcmp(sbi_key, "verify_client")) {
                                verify_client = ogs_yaml_iter_bool(&sbi_iter);
                            } else if (!strcmp(sbi_key,
                                        "verify_client_cacert")) {
                                verify_client_cacert =
                                    ogs_yaml_iter_value(&sbi_iter);
                            } else if (!strcmp(sbi_key, "option")) {
                                rv = ogs_app_config_parse_sockopt(
                                        &sbi_iter, &option);
                                if (rv != OGS_OK) return rv;
                                is_option = true;
                            } else
                                ogs_warn("unknown key `%s`", sbi_key);
                        }

                        if (scheme == OpenAPI_uri_scheme_NULL)
                            scheme = ogs_sbi_self()->tls.server.scheme;

                        if (!port) {
                            if (scheme == OpenAPI_uri_scheme_https)
                                port = OGS_SBI_HTTPS_PORT;
                            else if (scheme == OpenAPI_uri_scheme_http)
                                port = OGS_SBI_HTTP_PORT;
                            else
                                ogs_assert_if_reached();
                        }

                        addr = NULL;
                        for (i = 0; i < num; i++) {
                            rv = ogs_addaddrinfo(&addr,
                                    family, hostname[i], port, 0);
                            ogs_assert(rv == OGS_OK);
                        }

                        ogs_list_init(&list);
                        ogs_list_init(&list6);

                        if (addr) {
                            if (ogs_app()->parameter.no_ipv4 == 0)
                                ogs_socknode_add(
                                    &list, AF_INET, addr, NULL);
                            if (ogs_app()->parameter.no_ipv6 == 0)
                                ogs_socknode_add(
                                    &list6, AF_INET6, addr, NULL);
                            ogs_freeaddrinfo(addr);
                        }

                        if (dev) {
                            rv = ogs_socknode_probe(
                                ogs_app()->parameter.no_ipv4 ? NULL : &list,
                                ogs_app()->parameter.no_ipv6 ? NULL : &list6,
                                dev, port, NULL);
                            ogs_assert(rv == OGS_OK);
                        }

                        addr = NULL;
                        for (i = 0; i < num_of_advertise; i++) {
                            rv = ogs_addaddrinfo(&addr,
                                    family, advertise[i], port, 0);
                            ogs_assert(rv == OGS_OK);
                        }

                        node = ogs_list_first(&list);
                        if (node) {
                            server = ogs_sbi_server_add(scheme, node->addr,
                                        is_option ? &option : NULL);
                            ogs_assert(server);

                            if (addr && ogs_app()->parameter.no_ipv4 == 0)
                                ogs_sbi_server_set_advertise(
                                        server, AF_INET, addr);

                            if (verify_client == true)
                                server->verify_client = true;

                            if (verify_client_cacert) {
                                if (server->verify_client_cacert)
                                    ogs_free(server->verify_client_cacert);
                                server->verify_client_cacert =
                                    ogs_strdup(verify_client_cacert);
                                ogs_assert(server->verify_client_cacert);
                            }

                            if (server->verify_client == true &&
                                !server->verify_client_cacert) {
                                ogs_error("CLIENT verification enabled "
                                        "but no CA certificate");
                                return OGS_ERROR;
                            }

                            if (private_key) {
                                if (server->private_key)
                                    ogs_free(server->private_key);
                                server->private_key = ogs_strdup(private_key);
                                ogs_assert(server->private_key);
                            }
                            if (cert) {
                                if (server->cert)
                                    ogs_free(server->cert);
                                server->cert = ogs_strdup(cert);
                                ogs_assert(server->cert);
                            }

                            if (scheme == OpenAPI_uri_scheme_https) {
                                if (!server->private_key) {
                                    ogs_error("HTTPS scheme enabled "
                                            "but no server key");
                                    return OGS_ERROR;
                                }
                                if (!server->cert) {
                                    ogs_error("HTTPS scheme enabled "
                                            "but no server certificate");
                                    return OGS_ERROR;
                                }
                            }
                        }
                        node6 = ogs_list_first(&list6);
                        if (node6) {
                            server = ogs_sbi_server_add(scheme, node6->addr,
                                        is_option ? &option : NULL);
                            ogs_assert(server);

                            if (addr && ogs_app()->parameter.no_ipv6 == 0)
                                ogs_sbi_server_set_advertise(
                                        server, AF_INET6, addr);

                            if (verify_client == true)
                                server->verify_client = true;

                            if (verify_client_cacert) {
                                if (server->verify_client_cacert)
                                    ogs_free(server->verify_client_cacert);
                                server->verify_client_cacert =
                                    ogs_strdup(verify_client_cacert);
                                ogs_assert(server->verify_client_cacert);
                            }

                            if (server->verify_client == true &&
                                !server->verify_client_cacert) {
                                ogs_error("CLIENT verification enabled "
                                        "but no CA certificate");
                                return OGS_ERROR;
                            }

                            if (private_key) {
                                if (server->private_key)
                                    ogs_free(server->private_key);
                                server->private_key = ogs_strdup(private_key);
                                ogs_assert(server->private_key);
                            }
                            if (cert) {
                                if (server->cert)
                                    ogs_free(server->cert);
                                server->cert = ogs_strdup(cert);
                                ogs_assert(server->cert);
                            }

                            if (scheme == OpenAPI_uri_scheme_https) {
                                if (!server->private_key) {
                                    ogs_error("HTTPS scheme enabled "
                                            "but no server key");
                                    return OGS_ERROR;
                                }
                                if (!server->cert) {
                                    ogs_error("HTTPS scheme enabled "
                                            "but no server certificate");
                                    return OGS_ERROR;
                                }
                            }
                        }

                        if (addr)
                            ogs_freeaddrinfo(addr);

                        ogs_socknode_remove_all(&list);
                        ogs_socknode_remove_all(&list6);

                    } while (ogs_yaml_iter_type(&sbi_array) ==
                            YAML_SEQUENCE_NODE);

                    scheme = OpenAPI_uri_scheme_NULL;

                    ogs_list_for_each(&ogs_sbi_self()->server_list, server) {
                        if (scheme == OpenAPI_uri_scheme_NULL) {
                            scheme = server->scheme;
                            ogs_assert(scheme);
                        } else if (scheme != server->scheme) {
                            ogs_error("Different SCHEME is used in SBI Server");
                            return OGS_ERROR;
                        }
                    }

                } else if (ogs_app()->parameter.no_nrf == false &&
                        nrf && !strcmp(local_key, nrf)) {
                    ogs_yaml_iter_t nrf_array, nrf_iter;
                    ogs_yaml_iter_recurse(&local_iter, &nrf_array);
                    do {
                        ogs_sbi_client_t *client = NULL;
                        const char *uri = NULL;

                        bool insecure_skip_verify = false;
                        const char *cacert = NULL;

                        const char *client_private_key = NULL;
                        const char *client_cert = NULL;

                        if (ogs_yaml_iter_type(&nrf_array) ==
                                YAML_MAPPING_NODE) {
                            memcpy(&nrf_iter, &nrf_array,
                                    sizeof(ogs_yaml_iter_t));
                        } else if (ogs_yaml_iter_type(&nrf_array) ==
                            YAML_SEQUENCE_NODE) {
                            if (!ogs_yaml_iter_next(&nrf_array))
                                break;
                            ogs_yaml_iter_recurse(&nrf_array, &nrf_iter);
                        } else if (ogs_yaml_iter_type(&nrf_array) ==
                                YAML_SCALAR_NODE) {
                            break;
                        } else
                            ogs_assert_if_reached();

                        while (ogs_yaml_iter_next(&nrf_iter)) {
                            const char *nrf_key =
                                ogs_yaml_iter_key(&nrf_iter);
                            ogs_assert(nrf_key);
                            if (!strcmp(nrf_key, "uri")) {
                                uri = ogs_yaml_iter_value(&nrf_iter);
                            } else if (!strcmp(nrf_key,
                                        "insecure_skip_verify")) {
                                insecure_skip_verify =
                                    ogs_yaml_iter_bool(&nrf_iter);
                            } else if (!strcmp(nrf_key, "cacert")) {
                                cacert = ogs_yaml_iter_value(&nrf_iter);
                            } else if (!strcmp(nrf_key, "client_private_key")) {
                                client_private_key =
                                    ogs_yaml_iter_value(&nrf_iter);
                            } else if (!strcmp(nrf_key, "client_cert")) {
                                client_cert = ogs_yaml_iter_value(&nrf_iter);
                            } else
                                ogs_warn("unknown key `%s`", nrf_key);
                        }

                        if (uri) {
                            bool rc;

                            OpenAPI_uri_scheme_e scheme =
                                OpenAPI_uri_scheme_NULL;

                            char *fqdn = NULL;
                            uint16_t fqdn_port = 0;
                            ogs_sockaddr_t *addr = NULL, *addr6 = NULL;

                            rc = ogs_sbi_getaddr_from_uri(
                                    &scheme, &fqdn, &fqdn_port, &addr, &addr6,
                                    (char *)uri);
                            if (rc == false) {
                                if (!scheme)
                                    ogs_error("Invalid Scheme in URI[%s]", uri);
                                else
                                    ogs_error("Invalid URI[%s]", uri);

                                return OGS_ERROR;
                            }

                            if (NF_INSTANCE_CLIENT(self.nrf_instance)) {
                                ogs_warn("NRF has already been configured");

                                ogs_free(fqdn);
                                ogs_freeaddrinfo(addr);
                                ogs_freeaddrinfo(addr6);

                                return OGS_ERROR;
                            }

                            client = ogs_sbi_client_add(
                                    scheme, fqdn, fqdn_port, addr, addr6);
                            ogs_assert(client);
                            OGS_SBI_SETUP_CLIENT(self.nrf_instance, client);

                            if (insecure_skip_verify == true)
                                client->insecure_skip_verify = true;

                            if (cacert) {
                                if (client->cacert)
                                    ogs_free(client->cacert);
                                client->cacert = ogs_strdup(cacert);
                                ogs_assert(client->cacert);
                            }

                            if (client_private_key) {
                                if (client->private_key)
                                    ogs_free(client->private_key);
                                client->private_key =
                                    ogs_strdup(client_private_key);
                                ogs_assert(client->private_key);
                            }

                            if (client_cert) {
                                if (client->cert)
                                    ogs_free(client->cert);
                                client->cert = ogs_strdup(client_cert);
                                ogs_assert(client->cert);
                            }

                            if ((!client_private_key && client_cert) ||
                                (client_private_key && !client_cert)) {
                                ogs_error("Either the private key or "
                                        "certificate is missing.");
                                return OGS_ERROR;
                            }

                            ogs_free(fqdn);
                            ogs_freeaddrinfo(addr);
                            ogs_freeaddrinfo(addr6);
                        } else {
                            ogs_error("Invalid Mandatory [URI:%s]",
                                        uri ? uri : "NULL");
                        }
                    } while (ogs_yaml_iter_type(&nrf_array) ==
                            YAML_SEQUENCE_NODE);
                } else if (ogs_app()->parameter.no_scp == false &&
                    scp && !strcmp(local_key, scp)) {
                    ogs_yaml_iter_t scp_array, scp_iter;
                    ogs_yaml_iter_recurse(&local_iter, &scp_array);
                    do {
                        ogs_sbi_client_t *client = NULL;
                        const char *uri = NULL;

                        bool insecure_skip_verify = false;
                        const char *cacert = NULL;

                        const char *client_private_key = NULL;
                        const char *client_cert = NULL;

                        if (ogs_yaml_iter_type(&scp_array) ==
                                YAML_MAPPING_NODE) {
                            memcpy(&scp_iter, &scp_array,
                                    sizeof(ogs_yaml_iter_t));
                        } else if (ogs_yaml_iter_type(&scp_array) ==
                            YAML_SEQUENCE_NODE) {
                            if (!ogs_yaml_iter_next(&scp_array))
                                break;
                            ogs_yaml_iter_recurse(&scp_array, &scp_iter);
                        } else if (ogs_yaml_iter_type(&scp_array) ==
                                YAML_SCALAR_NODE) {
                            break;
                        } else
                            ogs_assert_if_reached();

                        while (ogs_yaml_iter_next(&scp_iter)) {
                            const char *scp_key =
                                ogs_yaml_iter_key(&scp_iter);
                            ogs_assert(scp_key);
                            if (!strcmp(scp_key, "uri")) {
                                uri = ogs_yaml_iter_value(&scp_iter);
                            } else if (!strcmp(scp_key,
                                        "insecure_skip_verify")) {
                                insecure_skip_verify =
                                    ogs_yaml_iter_bool(&scp_iter);
                            } else if (!strcmp(scp_key, "cacert")) {
                                cacert = ogs_yaml_iter_value(&scp_iter);
                            } else if (!strcmp(scp_key, "client_private_key")) {
                                client_private_key =
                                    ogs_yaml_iter_value(&scp_iter);
                            } else if (!strcmp(scp_key, "client_cert")) {
                                client_cert = ogs_yaml_iter_value(&scp_iter);
                            } else
                                ogs_warn("unknown key `%s`", scp_key);
                        }

                        if (uri) {
                            bool rc;
                            OpenAPI_uri_scheme_e scheme =
                                OpenAPI_uri_scheme_NULL;

                            char *fqdn = NULL;
                            uint16_t fqdn_port = 0;
                            ogs_sockaddr_t *addr = NULL, *addr6 = NULL;

                            rc = ogs_sbi_getaddr_from_uri(
                                    &scheme, &fqdn, &fqdn_port, &addr, &addr6,
                                    (char *)uri);
                            if (rc == false) {
                                if (!scheme)
                                    ogs_error("Invalid Scheme in URI[%s]", uri);
                                else
                                    ogs_error("Invalid URI[%s]", uri);

                                return OGS_ERROR;
                            }

                            if (NF_INSTANCE_CLIENT(self.scp_instance)) {
                                ogs_warn("SCP has already been configured");

                                ogs_free(fqdn);
                                ogs_freeaddrinfo(addr);
                                ogs_freeaddrinfo(addr6);

                                return OGS_ERROR;
                            }

                            client = ogs_sbi_client_add(
                                    scheme, fqdn, fqdn_port, addr, addr6);
                            ogs_assert(client);
                            OGS_SBI_SETUP_CLIENT(self.scp_instance, client);

                            if (insecure_skip_verify == true)
                                client->insecure_skip_verify = true;

                            if (cacert) {
                                if (client->cacert)
                                    ogs_free(client->cacert);
                                client->cacert = ogs_strdup(cacert);
                                ogs_assert(client->cacert);
                            }

                            if (client_private_key) {
                                if (client->private_key)
                                    ogs_free(client->private_key);
                                client->private_key =
                                    ogs_strdup(client_private_key);
                                ogs_assert(client->private_key);
                            }

                            if (client_cert) {
                                if (client->cert)
                                    ogs_free(client->cert);
                                client->cert = ogs_strdup(client_cert);
                                ogs_assert(client->cert);
                            }

                            if ((!client_private_key && client_cert) ||
                                (client_private_key && !client_cert)) {
                                ogs_error("Either the private key or "
                                        "certificate is missing.");
                                return OGS_ERROR;
                            }

                            ogs_free(fqdn);
                            ogs_freeaddrinfo(addr);
                            ogs_freeaddrinfo(addr6);
                        } else {
                            ogs_error("Invalid Mandatory [URI:%s]",
                                        uri ? uri : "NULL");
                        }
                    } while (ogs_yaml_iter_type(&scp_array) ==
                            YAML_SEQUENCE_NODE);
                } else if (!strcmp(local_key, "service_name")) {
                    ogs_yaml_iter_t service_name_iter;
                    ogs_yaml_iter_recurse(&local_iter, &service_name_iter);
                    ogs_assert(ogs_yaml_iter_type(
                                &service_name_iter) != YAML_MAPPING_NODE);

                    do {
                        const char *v = NULL;

                        if (ogs_yaml_iter_type(&service_name_iter) ==
                                YAML_SEQUENCE_NODE) {
                            if (!ogs_yaml_iter_next(&service_name_iter))
                                break;
                        }

                        v = ogs_yaml_iter_value(&service_name_iter);
                        if (v && strlen(v))
                            self.service_name[self.num_of_service_name++] = v;

                    } while (ogs_yaml_iter_type(
                                &service_name_iter) == YAML_SEQUENCE_NODE);

                } else if (!strcmp(local_key, "discovery")) {
                    ogs_yaml_iter_t discovery_iter;
                    ogs_yaml_iter_recurse(&local_iter, &discovery_iter);
                    while (ogs_yaml_iter_next(&discovery_iter)) {
                        const char *discovery_key =
                            ogs_yaml_iter_key(&discovery_iter);
                        ogs_assert(discovery_key);
                        if (!strcmp(discovery_key, "delegated")) {
                            const char *delegated =
                                ogs_yaml_iter_value(&discovery_iter);
                            if (!strcmp(delegated, "auto"))
                                self.discovery_config.delegated =
                                    OGS_SBI_DISCOVERY_DELEGATED_AUTO;
                            else if (!strcmp(delegated, "yes"))
                                self.discovery_config.delegated =
                                    OGS_SBI_DISCOVERY_DELEGATED_YES;
                            else if (!strcmp(delegated, "no"))
                                self.discovery_config.delegated =
                                    OGS_SBI_DISCOVERY_DELEGATED_NO;
                            else
                                ogs_warn("unknown 'delegated' value `%s`",
                                        delegated);
                        } else if (!strcmp(discovery_key, "option")) {
                            ogs_yaml_iter_t option_iter;
                            ogs_yaml_iter_recurse(
                                    &discovery_iter, &option_iter);

                            while (ogs_yaml_iter_next(&option_iter)) {
                                const char *option_key =
                                    ogs_yaml_iter_key(&option_iter);
                                ogs_assert(option_key);

                                if (!strcmp(option_key, "no_service_names")) {
                                    self.discovery_config.no_service_names =
                                        ogs_yaml_iter_bool(&option_iter);
                                } else if (!strcmp(option_key,
                                        "prefer_requester_nf_instance_id")) {
                                    self.discovery_config.
                                        prefer_requester_nf_instance_id =
                                            ogs_yaml_iter_bool(&option_iter);
                                } else
                                    ogs_warn("unknown key `%s`", option_key);
                            }
                        } else
                            ogs_warn("unknown key `%s`", discovery_key);
                    }
                }
            }
        }
    }

    rv = ogs_sbi_context_validation(local, nrf, scp);
    if (rv != OGS_OK) return rv;

    return OGS_OK;
}

int ogs_sbi_context_parse_hnet_config(ogs_yaml_iter_t *root_iter)
{
    int rv;
    ogs_yaml_iter_t hnet_array, hnet_iter;

    ogs_assert(root_iter);
    ogs_yaml_iter_recurse(root_iter, &hnet_array);
    do {
        uint8_t id = 0, scheme = 0;
        const char *filename = NULL;

        if (ogs_yaml_iter_type(&hnet_array) == YAML_MAPPING_NODE) {
            memcpy(&hnet_iter, &hnet_array, sizeof(ogs_yaml_iter_t));
        } else if (ogs_yaml_iter_type(&hnet_array) == YAML_SEQUENCE_NODE) {
            if (!ogs_yaml_iter_next(&hnet_array))
                break;
            ogs_yaml_iter_recurse(&hnet_array, &hnet_iter);
        } else if (ogs_yaml_iter_type(&hnet_array) == YAML_SCALAR_NODE) {
            break;
        } else
            ogs_assert_if_reached();

        while (ogs_yaml_iter_next(&hnet_iter)) {
            const char *hnet_key = ogs_yaml_iter_key(&hnet_iter);
            ogs_assert(hnet_key);
            if (!strcmp(hnet_key, "id")) {
                const char *v = ogs_yaml_iter_value(&hnet_iter);
                if (v) {
                    if (atoi(v) >= 1 && atoi(v) <= 254)
                        id = atoi(v);
                }
            } else if (!strcmp(hnet_key, "scheme")) {
                const char *v = ogs_yaml_iter_value(&hnet_iter);
                if (v) {
                    if (atoi(v) == 1 || atoi(v) == 2)
                        scheme = atoi(v);
                }
            } else if (!strcmp(hnet_key, "key")) {
                filename = ogs_yaml_iter_value(&hnet_iter);
            } else
                ogs_warn("unknown key `%s`", hnet_key);
        }

        if (id >= OGS_HOME_NETWORK_PKI_VALUE_MIN &&
            id <= OGS_HOME_NETWORK_PKI_VALUE_MAX &&
            filename) {
            if (scheme == OGS_PROTECTION_SCHEME_PROFILE_A) {
                rv = ogs_pem_decode_curve25519_key(
                        filename, self.hnet[id].key);
                if (rv == OGS_OK) {
                    self.hnet[id].avail = true;
                    self.hnet[id].scheme = scheme;
                } else {
                    ogs_error("ogs_pem_decode_curve25519_key"
                            "[%s] failed", filename);
                }
            } else if (scheme == OGS_PROTECTION_SCHEME_PROFILE_B) {
                rv = ogs_pem_decode_secp256r1_key(
                        filename, self.hnet[id].key);
                if (rv == OGS_OK) {
                    self.hnet[id].avail = true;
                    self.hnet[id].scheme = scheme;
                } else {
                    ogs_error("ogs_pem_decode_secp256r1_key[%s]"
                            " failed", filename);
                }
            } else
                ogs_error("Invalid scheme [%d]", scheme);
        } else
            ogs_error("Invalid home network configuration "
                    "[id:%d, filename:%s]", id, filename);
    } while (ogs_yaml_iter_type(&hnet_array) == YAML_SEQUENCE_NODE);

    return OGS_OK;
}

bool ogs_sbi_nf_service_is_available(const char *name)
{
    int i;

    ogs_assert(name);

    if (self.num_of_service_name == 0)
        /* If no service name is configured, all services are available */
        return true;

    for (i = 0; i < self.num_of_service_name; i++)
        /* Only services in the configuration are available */
        if (strcmp(self.service_name[i], name) == 0)
            return true;

    return false;
}

ogs_sbi_nf_instance_t *ogs_sbi_nf_instance_add(void)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL;

    ogs_pool_alloc(&nf_instance_pool, &nf_instance);
    ogs_assert(nf_instance);
    memset(nf_instance, 0, sizeof(ogs_sbi_nf_instance_t));

    ogs_debug("ogs_sbi_nf_instance_add()");

    OGS_OBJECT_REF(nf_instance);

    nf_instance->time.heartbeat_interval =
            ogs_app()->time.nf_instance.heartbeat_interval;

    nf_instance->priority = OGS_SBI_DEFAULT_PRIORITY;
    nf_instance->capacity = OGS_SBI_DEFAULT_CAPACITY;
    nf_instance->load = OGS_SBI_DEFAULT_LOAD;

    ogs_list_add(&ogs_sbi_self()->nf_instance_list, nf_instance);

    return nf_instance;
}

void ogs_sbi_nf_instance_set_id(ogs_sbi_nf_instance_t *nf_instance, char *id)
{
    ogs_assert(nf_instance);
    ogs_assert(id);

    nf_instance->id = ogs_strdup(id);
    ogs_assert(nf_instance->id);
}

void ogs_sbi_nf_instance_set_type(
        ogs_sbi_nf_instance_t *nf_instance, OpenAPI_nf_type_e nf_type)
{
    ogs_assert(nf_instance);
    ogs_assert(nf_type);

    nf_instance->nf_type = nf_type;
}

void ogs_sbi_nf_instance_set_status(
        ogs_sbi_nf_instance_t *nf_instance, OpenAPI_nf_status_e nf_status)
{
    ogs_assert(nf_instance);
    ogs_assert(nf_status);

    nf_instance->nf_status = nf_status;
}

void ogs_sbi_nf_instance_add_allowed_nf_type(
        ogs_sbi_nf_instance_t *nf_instance, OpenAPI_nf_type_e allowed_nf_type)
{
    ogs_assert(nf_instance);
    ogs_assert(allowed_nf_type);

    if (nf_instance->num_of_allowed_nf_type < OGS_SBI_MAX_NUM_OF_NF_TYPE) {
        nf_instance->allowed_nf_type[nf_instance->num_of_allowed_nf_type] =
            allowed_nf_type;
        nf_instance->num_of_allowed_nf_type++;
    }
}

bool ogs_sbi_nf_instance_is_allowed_nf_type(
        ogs_sbi_nf_instance_t *nf_instance, OpenAPI_nf_type_e allowed_nf_type)
{
    int i;

    ogs_assert(nf_instance);
    ogs_assert(allowed_nf_type);

    if (!nf_instance->num_of_allowed_nf_type) {
        return true;
    }

    for (i = 0; i < nf_instance->num_of_allowed_nf_type; i++) {
        if (nf_instance->allowed_nf_type[i] == allowed_nf_type)
            return true;
    }

    ogs_error("Not allowed nf-type[%s] in nf-instance[%s]",
            OpenAPI_nf_type_ToString(allowed_nf_type),
            OpenAPI_nf_type_ToString(nf_instance->nf_type));
    return false;
}

void ogs_sbi_nf_instance_clear(ogs_sbi_nf_instance_t *nf_instance)
{
    int i;

    ogs_assert(nf_instance);

    if (nf_instance->fqdn)
        ogs_free(nf_instance->fqdn);
    nf_instance->fqdn = NULL;

    for (i = 0; i < nf_instance->num_of_ipv4; i++) {
        if (nf_instance->ipv4[i])
            ogs_freeaddrinfo(nf_instance->ipv4[i]);
    }
    nf_instance->num_of_ipv4 = 0;

    for (i = 0; i < nf_instance->num_of_ipv6; i++) {
        if (nf_instance->ipv6[i])
            ogs_freeaddrinfo(nf_instance->ipv6[i]);
    }
    nf_instance->num_of_ipv6 = 0;

    nf_instance->num_of_allowed_nf_type = 0;
}

void ogs_sbi_nf_instance_remove(ogs_sbi_nf_instance_t *nf_instance)
{
    ogs_assert(nf_instance);

    ogs_debug("ogs_sbi_nf_instance_remove()");

    if (OGS_OBJECT_IS_REF(nf_instance)) {
        OGS_OBJECT_UNREF(nf_instance);
        return;
    }

    ogs_list_remove(&ogs_sbi_self()->nf_instance_list, nf_instance);

    ogs_sbi_nf_info_remove_all(&nf_instance->nf_info_list);

    ogs_sbi_nf_service_remove_all(nf_instance);

    ogs_sbi_nf_instance_clear(nf_instance);

    if (nf_instance->id) {
        ogs_sbi_subscription_data_remove_all_by_nf_instance_id(nf_instance->id);
        ogs_free(nf_instance->id);
    }

    if (nf_instance->client)
        ogs_sbi_client_remove(nf_instance->client);

    ogs_pool_free(&nf_instance_pool, nf_instance);
}

void ogs_sbi_nf_instance_remove_all(void)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL, *next_nf_instance = NULL;

    ogs_list_for_each_safe(
            &ogs_sbi_self()->nf_instance_list, next_nf_instance, nf_instance)
        ogs_sbi_nf_instance_remove(nf_instance);
}

ogs_sbi_nf_instance_t *ogs_sbi_nf_instance_find(char *id)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL;

    ogs_assert(id);

    ogs_list_for_each(&ogs_sbi_self()->nf_instance_list, nf_instance) {
        if (nf_instance->id && strcmp(nf_instance->id, id) == 0)
            break;
    }

    return nf_instance;
}

ogs_sbi_nf_instance_t *ogs_sbi_nf_instance_find_by_discovery_param(
        OpenAPI_nf_type_e target_nf_type,
        OpenAPI_nf_type_e requester_nf_type,
        ogs_sbi_discovery_option_t *discovery_option)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL;

    ogs_assert(target_nf_type);
    ogs_assert(requester_nf_type);

    ogs_list_for_each(&ogs_sbi_self()->nf_instance_list, nf_instance) {
        if (ogs_sbi_discovery_param_is_matched(
                    nf_instance, target_nf_type, requester_nf_type,
                    discovery_option) == false)
            continue;

        return nf_instance;
    }

    return NULL;
}

ogs_sbi_nf_instance_t *ogs_sbi_nf_instance_find_by_service_type(
        ogs_sbi_service_type_e service_type,
        OpenAPI_nf_type_e requester_nf_type)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL;
    ogs_sbi_discovery_option_t *discovery_option = NULL;

    OpenAPI_nf_type_e target_nf_type = OpenAPI_nf_type_NULL;
    char *service_name = NULL;

    ogs_assert(requester_nf_type);
    ogs_assert(service_type);
    target_nf_type = ogs_sbi_service_type_to_nf_type(service_type);
    ogs_assert(target_nf_type);
    service_name = (char *)ogs_sbi_service_type_to_name(service_type);
    ogs_assert(service_name);

    discovery_option = ogs_sbi_discovery_option_new();
    ogs_assert(discovery_option);
    ogs_sbi_discovery_option_add_service_names(discovery_option, service_name);

    nf_instance = ogs_sbi_nf_instance_find_by_discovery_param(
            target_nf_type, requester_nf_type, discovery_option);

    ogs_sbi_discovery_option_free(discovery_option);

    return nf_instance;
}

bool ogs_sbi_nf_instance_maximum_number_is_reached(void)
{
    return nf_instance_pool.avail <= 0;
}

ogs_sbi_nf_service_t *ogs_sbi_nf_service_add(
        ogs_sbi_nf_instance_t *nf_instance,
        char *id, const char *name, OpenAPI_uri_scheme_e scheme)
{
    ogs_sbi_nf_service_t *nf_service = NULL;

    ogs_assert(nf_instance);
    ogs_assert(id);
    ogs_assert(name);

    ogs_pool_alloc(&nf_service_pool, &nf_service);
    ogs_assert(nf_service);
    memset(nf_service, 0, sizeof(ogs_sbi_nf_service_t));

    nf_service->id = ogs_strdup(id);
    ogs_assert(nf_service->id);
    nf_service->name = ogs_strdup(name);
    ogs_assert(nf_service->name);
    nf_service->scheme = scheme;
    ogs_assert(nf_service->scheme);

    nf_service->status = OpenAPI_nf_service_status_REGISTERED;

    nf_service->priority = OGS_SBI_DEFAULT_PRIORITY;
    nf_service->capacity = OGS_SBI_DEFAULT_CAPACITY;
    nf_service->load = OGS_SBI_DEFAULT_LOAD;

    nf_service->nf_instance = nf_instance;

    ogs_list_add(&nf_instance->nf_service_list, nf_service);

    return nf_service;
}

void ogs_sbi_nf_service_add_version(ogs_sbi_nf_service_t *nf_service,
        const char *in_uri, const char *full, const char *expiry)
{
    ogs_assert(nf_service);

    ogs_assert(in_uri);
    ogs_assert(full);

    if (nf_service->num_of_version < OGS_SBI_MAX_NUM_OF_SERVICE_VERSION) {
        nf_service->version[nf_service->num_of_version].in_uri =
            ogs_strdup(in_uri);
        ogs_assert(nf_service->version[nf_service->num_of_version].in_uri);
        nf_service->version[nf_service->num_of_version].full =
            ogs_strdup(full);
        ogs_assert(nf_service->version[nf_service->num_of_version].full);
        if (expiry) {
            nf_service->version[nf_service->num_of_version].expiry =
                ogs_strdup(expiry);
            ogs_assert(
                nf_service->version[nf_service->num_of_version].expiry);

        }
        nf_service->num_of_version++;
    }
}

void ogs_sbi_nf_service_add_allowed_nf_type(
        ogs_sbi_nf_service_t *nf_service, OpenAPI_nf_type_e allowed_nf_type)
{
    ogs_assert(nf_service);
    ogs_assert(allowed_nf_type);

    if (nf_service->num_of_allowed_nf_type < OGS_SBI_MAX_NUM_OF_NF_TYPE) {
        nf_service->allowed_nf_type[nf_service->num_of_allowed_nf_type] =
            allowed_nf_type;
        nf_service->num_of_allowed_nf_type++;
    }
}

bool ogs_sbi_nf_service_is_allowed_nf_type(
        ogs_sbi_nf_service_t *nf_service, OpenAPI_nf_type_e allowed_nf_type)
{
    int i;

    ogs_assert(nf_service);
    ogs_assert(allowed_nf_type);

    if (!nf_service->num_of_allowed_nf_type) {
        return true;
    }

    for (i = 0; i < nf_service->num_of_allowed_nf_type; i++) {
        if (nf_service->allowed_nf_type[i] == allowed_nf_type)
            return true;
    }

    ogs_assert(nf_service->name);
    ogs_error("Not allowed nf-type[%s] in nf-service[%s]",
            OpenAPI_nf_type_ToString(allowed_nf_type),
            nf_service->name);
    return false;
}

void ogs_sbi_nf_service_clear(ogs_sbi_nf_service_t *nf_service)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL;
    int i;

    ogs_assert(nf_service);
    nf_instance = nf_service->nf_instance;
    ogs_assert(nf_instance);

    if (nf_service->fqdn)
        ogs_free(nf_service->fqdn);
    nf_service->fqdn = NULL;

    for (i = 0; i < nf_service->num_of_version; i++) {
        if (nf_service->version[i].in_uri)
            ogs_free(nf_service->version[i].in_uri);
        if (nf_service->version[i].full)
            ogs_free(nf_service->version[i].full);
        if (nf_service->version[i].expiry)
            ogs_free(nf_service->version[i].expiry);
    }
    nf_service->num_of_version = 0;

    for (i = 0; i < nf_service->num_of_addr; i++) {
        if (nf_service->addr[i].ipv4)
            ogs_freeaddrinfo(nf_service->addr[i].ipv4);
        if (nf_service->addr[i].ipv6)
            ogs_freeaddrinfo(nf_service->addr[i].ipv6);
    }
    nf_service->num_of_addr = 0;

    nf_service->num_of_allowed_nf_type = 0;
}

void ogs_sbi_nf_service_remove(ogs_sbi_nf_service_t *nf_service)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL;

    ogs_assert(nf_service);
    nf_instance = nf_service->nf_instance;
    ogs_assert(nf_instance);

    ogs_list_remove(&nf_instance->nf_service_list, nf_service);

    ogs_assert(nf_service->id);
    ogs_free(nf_service->id);

    ogs_assert(nf_service->name);
    ogs_free(nf_service->name);

    ogs_sbi_nf_service_clear(nf_service);

    if (nf_service->client)
        ogs_sbi_client_remove(nf_service->client);

    ogs_pool_free(&nf_service_pool, nf_service);
}

void ogs_sbi_nf_service_remove_all(ogs_sbi_nf_instance_t *nf_instance)
{
    ogs_sbi_nf_service_t *nf_service = NULL, *next_nf_service = NULL;

    ogs_assert(nf_instance);

    ogs_list_for_each_safe(&nf_instance->nf_service_list,
            next_nf_service, nf_service)
        ogs_sbi_nf_service_remove(nf_service);
}

ogs_sbi_nf_service_t *ogs_sbi_nf_service_find_by_id(
        ogs_sbi_nf_instance_t *nf_instance, char *id)
{
    ogs_sbi_nf_service_t *nf_service = NULL;

    ogs_assert(nf_instance);
    ogs_assert(id);

    ogs_list_for_each(&nf_instance->nf_service_list, nf_service) {
        ogs_assert(nf_service->id);
        if (strcmp(nf_service->id, id) == 0)
            break;
    }

    return nf_service;
}

ogs_sbi_nf_service_t *ogs_sbi_nf_service_find_by_name(
        ogs_sbi_nf_instance_t *nf_instance, char *name)
{
    ogs_sbi_nf_service_t *nf_service = NULL;

    ogs_assert(nf_instance);
    ogs_assert(name);

    ogs_list_for_each(&nf_instance->nf_service_list, nf_service) {
        ogs_assert(nf_service->name);
        if (strcmp(nf_service->name, name) == 0)
            return nf_service;
    }

    return NULL;
}

ogs_sbi_nf_info_t *ogs_sbi_nf_info_add(
        ogs_list_t *list, OpenAPI_nf_type_e nf_type)
{
    ogs_sbi_nf_info_t *nf_info = NULL;

    ogs_assert(list);
    ogs_assert(nf_type);

    ogs_pool_alloc(&nf_info_pool, &nf_info);
    if (!nf_info) {
        ogs_fatal("ogs_pool_alloc() failed");
        return NULL;
    }
    memset(nf_info, 0, sizeof(*nf_info));

    nf_info->nf_type = nf_type;

    ogs_list_add(list, nf_info);

    return nf_info;
}

static void amf_info_free(ogs_sbi_amf_info_t *amf_info)
{
    /* Nothing */
}

static void smf_info_free(ogs_sbi_smf_info_t *smf_info)
{
    int i, j;
    ogs_assert(smf_info);

    for (i = 0; i < smf_info->num_of_slice; i++) {
        for (j = 0; j < smf_info->slice[i].num_of_dnn; j++)
            ogs_free(smf_info->slice[i].dnn[j]);
        smf_info->slice[i].num_of_dnn = 0;
    }
    smf_info->num_of_slice = 0;
    smf_info->num_of_nr_tai = 0;
    smf_info->num_of_nr_tai_range = 0;

    ogs_pool_free(&smf_info_pool, smf_info);
}

static void scp_info_free(ogs_sbi_scp_info_t *scp_info)
{
    int i;
    for (i = 0; i < scp_info->num_of_domain; i++) {
        if (scp_info->domain[i].name)
            ogs_free(scp_info->domain[i].name);
        if (scp_info->domain[i].fqdn)
            ogs_free(scp_info->domain[i].fqdn);
    }
    scp_info->num_of_domain = 0;
}

static void sepp_info_free(ogs_sbi_sepp_info_t *sepp_info)
{
}

void ogs_sbi_nf_info_remove(ogs_list_t *list, ogs_sbi_nf_info_t *nf_info)
{
    ogs_assert(list);
    ogs_assert(nf_info);

    ogs_list_remove(list, nf_info);

    switch(nf_info->nf_type) {
    case OpenAPI_nf_type_AMF:
        amf_info_free(&nf_info->amf);
        break;
    case OpenAPI_nf_type_SMF:
        smf_info_free(&nf_info->smf);
        break;
    case OpenAPI_nf_type_SCP:
        scp_info_free(&nf_info->scp);
        break;
    case OpenAPI_nf_type_SEPP:
        sepp_info_free(&nf_info->sepp);
        break;
    default:
        ogs_fatal("Not implemented NF-type[%s]",
                OpenAPI_nf_type_ToString(nf_info->nf_type));
        ogs_assert_if_reached();
    }

    ogs_pool_free(&nf_info_pool, nf_info);
}

void ogs_sbi_nf_info_remove_all(ogs_list_t *list)
{
    ogs_sbi_nf_info_t *nf_info = NULL, *next_nf_info = NULL;

    ogs_assert(list);

    ogs_list_for_each_safe(list, next_nf_info, nf_info)
        ogs_sbi_nf_info_remove(list, nf_info);
}

ogs_sbi_nf_info_t *ogs_sbi_nf_info_find(
        ogs_list_t *list, OpenAPI_nf_type_e nf_type)
{
    ogs_sbi_nf_info_t *nf_info = NULL;

    ogs_assert(list);
    ogs_assert(nf_type);

    ogs_list_for_each(list, nf_info) {
        if (nf_info->nf_type == nf_type)
            return nf_info;
    }

    return NULL;
}

void ogs_sbi_nf_instance_build_default(ogs_sbi_nf_instance_t *nf_instance)
{
    ogs_sbi_server_t *server = NULL;
    char *hostname = NULL;

    ogs_assert(nf_instance);

    ogs_sbi_nf_instance_set_status(nf_instance, OpenAPI_nf_status_REGISTERED);

    hostname = NULL;
    ogs_list_for_each(&ogs_sbi_self()->server_list, server) {
        ogs_sockaddr_t *advertise = NULL;

        advertise = server->advertise;
        if (!advertise)
            advertise = server->node.addr;
        ogs_assert(advertise);

        /* First FQDN is selected */
        if (!hostname) {
            hostname = ogs_gethostname(advertise);
            if (hostname)
                continue;
        }

        if (nf_instance->num_of_ipv4 < OGS_SBI_MAX_NUM_OF_IP_ADDRESS) {
            ogs_sockaddr_t *addr = NULL;
            ogs_assert(OGS_OK == ogs_copyaddrinfo(&addr, advertise));
            ogs_assert(addr);

            if (addr->ogs_sa_family == AF_INET) {
                nf_instance->ipv4[nf_instance->num_of_ipv4] = addr;
                nf_instance->num_of_ipv4++;
            } else if (addr->ogs_sa_family == AF_INET6) {
                nf_instance->ipv6[nf_instance->num_of_ipv6] = addr;
                nf_instance->num_of_ipv6++;
            } else
                ogs_assert_if_reached();
        }
    }

    if (hostname) {
        nf_instance->fqdn = ogs_strdup(hostname);
        ogs_assert(nf_instance->fqdn);
    }

    nf_instance->time.heartbeat_interval =
            ogs_app()->time.nf_instance.heartbeat_interval;

    if (ogs_app()->num_of_serving_plmn_id) {
        memcpy(nf_instance->plmn_id, ogs_app()->serving_plmn_id,
                sizeof(nf_instance->plmn_id));
        nf_instance->num_of_plmn_id = ogs_app()->num_of_serving_plmn_id;
    }
}

ogs_sbi_nf_service_t *ogs_sbi_nf_service_build_default(
        ogs_sbi_nf_instance_t *nf_instance, const char *name)
{
    ogs_sbi_server_t *server = NULL;
    ogs_sbi_nf_service_t *nf_service = NULL;
    ogs_uuid_t uuid;
    char id[OGS_UUID_FORMATTED_LENGTH + 1];
    char *hostname = NULL;

    OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;

    ogs_assert(nf_instance);
    ogs_assert(name);

    ogs_uuid_get(&uuid);
    ogs_uuid_format(id, &uuid);

    server = ogs_list_first(&ogs_sbi_self()->server_list);
    ogs_assert(server);

    scheme = server->scheme;
    ogs_assert(scheme);

    nf_service = ogs_sbi_nf_service_add(nf_instance, id, name, scheme);
    ogs_assert(nf_service);

    hostname = NULL;
    ogs_list_for_each(&ogs_sbi_self()->server_list, server) {
        ogs_sockaddr_t *advertise = NULL;

        advertise = server->advertise;
        if (!advertise)
            advertise = server->node.addr;
        ogs_assert(advertise);

        /* First FQDN is selected */
        if (!hostname) {
            hostname = ogs_gethostname(advertise);
            if (hostname)
                continue;
        }

        if (nf_service->num_of_addr < OGS_SBI_MAX_NUM_OF_IP_ADDRESS) {
            bool is_port = true;
            int port = 0;
            ogs_sockaddr_t *addr = NULL;
            ogs_assert(OGS_OK == ogs_copyaddrinfo(&addr, advertise));
            ogs_assert(addr);

            port = OGS_PORT(addr);
            if (nf_service->scheme == OpenAPI_uri_scheme_https) {
                if (port == OGS_SBI_HTTPS_PORT) is_port = false;
            } else if (nf_service->scheme == OpenAPI_uri_scheme_http) {
                if (port == OGS_SBI_HTTP_PORT) is_port = false;
            }

            nf_service->addr[nf_service->num_of_addr].is_port = is_port;
            nf_service->addr[nf_service->num_of_addr].port = port;
            if (addr->ogs_sa_family == AF_INET) {
                nf_service->addr[nf_service->num_of_addr].ipv4 = addr;
            } else if (addr->ogs_sa_family == AF_INET6) {
                nf_service->addr[nf_service->num_of_addr].ipv6 = addr;
            } else
                ogs_assert_if_reached();

            nf_service->num_of_addr++;
        }
    }

    if (hostname) {
        nf_service->fqdn = ogs_strdup(hostname);
        ogs_assert(nf_service->fqdn);
    }

    ogs_info("NF Service [%s]", nf_service->name);

    return nf_service;
}

static ogs_sbi_client_t *nf_instance_find_client(
        ogs_sbi_nf_instance_t *nf_instance)
{
    ogs_sbi_client_t *client = NULL;
    ogs_sockaddr_t *addr = NULL, *addr6 = NULL;
    OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;

    ogs_sbi_nf_info_t *nf_info = NULL;
    uint16_t port = 0;

    scheme = ogs_sbi_self()->tls.client.scheme;
    ogs_assert(scheme);

    switch (nf_instance->nf_type) {
    case OpenAPI_nf_type_SEPP:
        nf_info = ogs_sbi_nf_info_find(
                    &nf_instance->nf_info_list, nf_instance->nf_type);
        if (nf_info) {
            if (scheme == OpenAPI_uri_scheme_https)
                port = nf_info->sepp.https.port;
            else if (scheme == OpenAPI_uri_scheme_http)
                port = nf_info->sepp.http.port;
            else
                ogs_error("Unknown scheme [%d]", scheme);
        }
        break;
    case OpenAPI_nf_type_SCP:
        nf_info = ogs_sbi_nf_info_find(
                    &nf_instance->nf_info_list, nf_instance->nf_type);
        if (nf_info) {
            if (scheme == OpenAPI_uri_scheme_https)
                port = nf_info->scp.https.port;
            else if (scheme == OpenAPI_uri_scheme_http)
                port = nf_info->scp.http.port;
            else
                ogs_error("Unknown scheme [%d]", scheme);
        }
        break;
    default:
        break;
    }

    /* At this point, CLIENT selection method is very simple. */
    if (nf_instance->num_of_ipv4) addr = nf_instance->ipv4[0];
    if (nf_instance->num_of_ipv6) addr6 = nf_instance->ipv6[0];

    if (port) {
        if (addr)
            addr->ogs_sin_port = htobe16(port);
        if (addr6)
            addr6->ogs_sin_port = htobe16(port);
    }

    if (nf_instance->fqdn || addr || addr6) {
        client = ogs_sbi_client_find(
                scheme, nf_instance->fqdn, port, addr, addr6);
        if (!client) {
            client = ogs_sbi_client_add(
                    scheme, nf_instance->fqdn, port, addr, addr6);
            ogs_assert(client);
        }
    }

    return client;
}

static void nf_service_associate_client(ogs_sbi_nf_service_t *nf_service)
{
    ogs_sbi_client_t *client = NULL;
    ogs_sockaddr_t *addr = NULL, *addr6 = NULL;

    ogs_assert(nf_service->scheme);

    /* At this point, CLIENT selection method is very simple. */
    if (nf_service->num_of_addr) {
        addr = nf_service->addr[0].ipv4;
        addr6 = nf_service->addr[0].ipv6;
    }

    if (nf_service->fqdn || addr || addr6) {
        client = ogs_sbi_client_find(
                nf_service->scheme, nf_service->fqdn, 0, addr, addr6);
        if (!client) {
            client = ogs_sbi_client_add(
                    nf_service->scheme, nf_service->fqdn, 0, addr, addr6);
            ogs_assert(client);
        }
    }

    if (client)
        OGS_SBI_SETUP_CLIENT(nf_service, client);
}

static void nf_service_associate_client_all(ogs_sbi_nf_instance_t *nf_instance)
{
    ogs_sbi_nf_service_t *nf_service = NULL;

    ogs_assert(nf_instance);

    ogs_list_for_each(&nf_instance->nf_service_list, nf_service)
        nf_service_associate_client(nf_service);
}

bool ogs_sbi_discovery_option_is_matched(
        ogs_sbi_nf_instance_t *nf_instance,
        OpenAPI_nf_type_e requester_nf_type,
        ogs_sbi_discovery_option_t *discovery_option)
{
    ogs_assert(nf_instance);
    ogs_assert(requester_nf_type);
    ogs_assert(discovery_option);

    if (discovery_option->target_nf_instance_id &&
        nf_instance->id && strcmp(nf_instance->id,
            discovery_option->target_nf_instance_id) != 0) {
        return false;
    }

    if (discovery_option->num_of_service_names) {
        if (ogs_sbi_discovery_option_service_names_is_matched(
                    nf_instance, requester_nf_type, discovery_option) == false)
            return false;
    }

    if (discovery_option->num_of_target_plmn_list) {
        if (ogs_sbi_discovery_option_target_plmn_list_is_matched(
                    nf_instance, discovery_option) == false)
            return false;
    }

    return true;
}

bool ogs_sbi_discovery_option_service_names_is_matched(
        ogs_sbi_nf_instance_t *nf_instance,
        OpenAPI_nf_type_e requester_nf_type,
        ogs_sbi_discovery_option_t *discovery_option)
{
    ogs_sbi_nf_service_t *nf_service = NULL;
    int i;

    ogs_assert(nf_instance);
    ogs_assert(requester_nf_type);
    ogs_assert(discovery_option);

    ogs_list_for_each(&nf_instance->nf_service_list, nf_service) {
        for (i = 0; i < discovery_option->num_of_service_names; i++) {
            if (nf_service->name &&
                discovery_option->service_names[i] &&
                strcmp(nf_service->name,
                    discovery_option->service_names[i]) == 0) {
                if (ogs_sbi_nf_service_is_allowed_nf_type(
                        nf_service, requester_nf_type) == true) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool ogs_sbi_discovery_param_serving_plmn_list_is_matched(
        ogs_sbi_nf_instance_t *nf_instance)
{
    int i, j;

    ogs_assert(nf_instance);

    /*
     * The PLMN-ID is optional and may not be set.
     *
     * Does not compare if serving PLMN-ID is not set or NF-Instance is not set.
     */
    if (ogs_app()->num_of_serving_plmn_id == 0 ||
            nf_instance->num_of_plmn_id == 0)
        return true;

    for (i = 0; i < nf_instance->num_of_plmn_id; i++) {
        for (j = 0; j < ogs_app()->num_of_serving_plmn_id; j++) {
            if (memcmp(&nf_instance->plmn_id[i], &ogs_app()->serving_plmn_id[j],
                       OGS_PLMN_ID_LEN) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool ogs_sbi_discovery_option_requester_plmn_list_is_matched(
        ogs_sbi_nf_instance_t *nf_instance,
        ogs_sbi_discovery_option_t *discovery_option)
{
    int i, j;

    ogs_assert(nf_instance);
    ogs_assert(discovery_option);

    for (i = 0; i < nf_instance->num_of_plmn_id; i++) {
        for (j = 0; j < discovery_option->num_of_requester_plmn_list; j++) {
            if (memcmp(&nf_instance->plmn_id[i],
                       &discovery_option->requester_plmn_list[j],
                       OGS_PLMN_ID_LEN) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool ogs_sbi_discovery_option_target_plmn_list_is_matched(
        ogs_sbi_nf_instance_t *nf_instance,
        ogs_sbi_discovery_option_t *discovery_option)
{
    int i, j;

    ogs_assert(nf_instance);
    ogs_assert(discovery_option);

    for (i = 0; i < nf_instance->num_of_plmn_id; i++) {
        for (j = 0; j < discovery_option->num_of_target_plmn_list; j++) {
            if (memcmp(&nf_instance->plmn_id[i],
                       &discovery_option->target_plmn_list[j],
                       OGS_PLMN_ID_LEN) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool ogs_sbi_discovery_param_is_matched(
        ogs_sbi_nf_instance_t *nf_instance,
        OpenAPI_nf_type_e target_nf_type,
        OpenAPI_nf_type_e requester_nf_type,
        ogs_sbi_discovery_option_t *discovery_option)
{
    ogs_assert(nf_instance);
    ogs_assert(target_nf_type);
    ogs_assert(requester_nf_type);

    if (NF_INSTANCE_EXCLUDED_FROM_DISCOVERY(nf_instance))
        return false;

    if (!OGS_FSM_CHECK(&nf_instance->sm, ogs_sbi_nf_state_registered))
        return false;

    if (nf_instance->nf_type != target_nf_type)
        return false;

    /*
     * For the same PLMN, The target-plmn-list may not be included
     * in discovery request.
     *
     * If the Serving PLMN needs to be discovered, but the target-plmn-list
     * is not included, the NF of the Home PLMN can be discovered.
     *
     * To avoid this situation, if the target-plmn-list is not included
     * and the serving PLMN is known, it is compared first.
     *
     * Refer to the following standard for this issue.
     *
     * TS29.510
     * 6.2 Nnrf_NFDiscovery Service API
     * 6.2.3 Resources
     * Table 6.2.3.2.3.1-1: URI query parameters supported
     * by the GET method on this resource
     *
     * NAME: target-plmn-list
     * Data type: array(PlmnId)
     * P: C
     * Cardinality: 1..N
     *
     * This IE shall be included when NF services in a different PLMN,
     * or NF services of specific PLMN ID(s) in a same PLMN
     * comprising multiple PLMN IDs, need to be discovered.
     * When included, this IE shall contain the PLMN ID of the target NF.
     * If more than one PLMN ID is included, NFs from any PLMN ID present
     * in the list matches the query parameter. This IE shall also
     * be included in SNPN scenarios, when the entity owning
     * the subscription, the Credentials Holder
     * (see clause 5.30.2.9 in 3GPP TS 23.501 [2]) is a PLMN.
     *
     * For inter-PLMN service discovery, at most 1 PLMN ID shall
     * be included in the list; it shall be included
     * in the service discovery from the NF in the source PLMN sent
     * to the NRF in the same PLMN, while it may be absent
     * in the service discovery request sent from the source NRF
     * to the target NRF. In such case, if the NRF receives more than
     * 1 PLMN ID, it shall only consider the first element of the array,
     * and ignore the rest.
     */
    if (!discovery_option || !discovery_option->num_of_target_plmn_list) {
        if (ogs_sbi_discovery_param_serving_plmn_list_is_matched(
                nf_instance) == false)
            return false;
    }

    if (discovery_option) {
        if (ogs_sbi_discovery_option_is_matched(
            nf_instance, requester_nf_type, discovery_option) == false)
        return false;
    }

    return true;
}

void ogs_sbi_client_associate(ogs_sbi_nf_instance_t *nf_instance)
{
    ogs_sbi_client_t *client = NULL;

    ogs_assert(nf_instance);

    client = nf_instance_find_client(nf_instance);
    ogs_assert(client);

    OGS_SBI_SETUP_CLIENT(nf_instance, client);

    nf_service_associate_client_all(nf_instance);
}

int ogs_sbi_default_client_port(OpenAPI_uri_scheme_e scheme)
{
    if (scheme == OpenAPI_uri_scheme_NULL)
        scheme = ogs_sbi_self()->tls.client.scheme;

    return scheme == OpenAPI_uri_scheme_https ?
            OGS_SBI_HTTPS_PORT : OGS_SBI_HTTP_PORT;
}

ogs_sbi_client_t *ogs_sbi_client_find_by_service_name(
        ogs_sbi_nf_instance_t *nf_instance, char *name, char *version)
{
    ogs_sbi_nf_service_t *nf_service = NULL;
    int i;

    ogs_assert(nf_instance);
    ogs_assert(name);
    ogs_assert(version);

    ogs_list_for_each(&nf_instance->nf_service_list, nf_service) {
        ogs_assert(nf_service->name);
        if (strcmp(nf_service->name, name) == 0) {
            for (i = 0; i < nf_service->num_of_version; i++) {
                if (strcmp(nf_service->version[i].in_uri, version) == 0) {
                    return nf_service->client;
                }
            }
        }
    }

    return nf_instance->client;
}

ogs_sbi_client_t *ogs_sbi_client_find_by_service_type(
        ogs_sbi_nf_instance_t *nf_instance,
        ogs_sbi_service_type_e service_type)
{
    ogs_sbi_nf_service_t *nf_service = NULL;

    ogs_assert(nf_instance);
    ogs_assert(service_type);

    ogs_list_for_each(&nf_instance->nf_service_list, nf_service) {
        ogs_assert(nf_service->name);
        if (ogs_sbi_service_type_from_name(nf_service->name) == service_type)
            return nf_service->client;
    }

    return nf_instance->client;
}

void ogs_sbi_object_free(ogs_sbi_object_t *sbi_object)
{
    int i;

    ogs_assert(sbi_object);

    if (ogs_list_count(&sbi_object->xact_list))
        ogs_error("SBI running [%d]", ogs_list_count(&sbi_object->xact_list));

    for (i = 0; i < OGS_SBI_MAX_NUM_OF_SERVICE_TYPE; i++) {
        ogs_sbi_nf_instance_t *nf_instance =
            sbi_object->service_type_array[i].nf_instance;
        if (nf_instance)
            ogs_sbi_nf_instance_remove(nf_instance);
    }
    for (i = 0; i < OGS_SBI_MAX_NUM_OF_NF_TYPE; i++) {
        ogs_sbi_nf_instance_t *nf_instance =
            sbi_object->nf_type_array[i].nf_instance;
        if (nf_instance)
            ogs_sbi_nf_instance_remove(nf_instance);
    }
}

ogs_sbi_xact_t *ogs_sbi_xact_add(
        ogs_sbi_object_t *sbi_object,
        ogs_sbi_service_type_e service_type,
        ogs_sbi_discovery_option_t *discovery_option,
        ogs_sbi_build_f build, void *context, void *data)
{
    ogs_sbi_xact_t *xact = NULL;

    ogs_assert(sbi_object);

    ogs_pool_alloc(&xact_pool, &xact);
    if (!xact) {
        ogs_error("ogs_pool_alloc() failed");
        return NULL;
    }
    memset(xact, 0, sizeof(ogs_sbi_xact_t));

    xact->sbi_object = sbi_object;
    xact->service_type = service_type;
    xact->requester_nf_type = NF_INSTANCE_TYPE(ogs_sbi_self()->nf_instance);
    ogs_assert(xact->requester_nf_type);

    /*
     * Insert one service-name in the discovery option in the function below.
     *
     * - ogs_sbi_xact_add()
     * - ogs_sbi_send_notification_request()
     */
    if (!discovery_option) {
        discovery_option = ogs_sbi_discovery_option_new();
        ogs_assert(discovery_option);

        /* ALWAYS add Service-MAP to requester-features in Discovery Option */
        OGS_SBI_FEATURES_SET(discovery_option->requester_features,
                OGS_SBI_NNRF_DISC_SERVICE_MAP);
    }

    if (!discovery_option->num_of_service_names) {
        ogs_sbi_discovery_option_add_service_names(
                discovery_option,
                (char *)ogs_sbi_service_type_to_name(service_type));
    }
    xact->discovery_option = discovery_option;

    xact->t_response = ogs_timer_add(
            ogs_app()->timer_mgr, ogs_timer_sbi_client_wait_expire, xact);
    if (!xact->t_response) {
        ogs_error("ogs_timer_add() failed");

        if (xact->discovery_option)
            ogs_sbi_discovery_option_free(xact->discovery_option);
        ogs_pool_free(&xact_pool, xact);

        return NULL;
    }

    ogs_timer_start(xact->t_response,
            ogs_app()->time.message.sbi.client_wait_duration);

    if (build) {
        xact->request = (*build)(context, data);
        if (!xact->request) {
            ogs_error("SBI build failed");

            if (xact->discovery_option)
                ogs_sbi_discovery_option_free(xact->discovery_option);

            ogs_timer_delete(xact->t_response);
            ogs_pool_free(&xact_pool, xact);

            return NULL;
        }
        if (!xact->request->h.uri) {
            const char *service_name = NULL;

            ogs_assert(xact->service_type);
            service_name = ogs_sbi_service_type_to_name(xact->service_type);
            ogs_assert(service_name);
            ogs_assert(xact->request->h.service.name);

            /*
             * Make sure the service matches
             * between discover and build functions:
             *
             * DISCOVER : amf_ue_sbi_discover_and_send(
             *              OGS_SBI_SERVICE_TYPE_NPCF_AM_POLICY_CONTROL,
             * BUILD    : amf_npcf_am_policy_control_build_create()
             *            message.h.service.name =
             *              (char *)OGS_SBI_SERVICE_NAME_NPCF_AM_POLICY_CONTROL;
             */

            if (strcmp(service_name, xact->request->h.service.name) != 0) {
                ogs_fatal("[%s:%d] is not the same with [%s]",
                            service_name, xact->service_type,
                            xact->request->h.service.name);
                ogs_assert_if_reached();
            }
        }
    }

    ogs_list_add(&sbi_object->xact_list, xact);

    return xact;
}

void ogs_sbi_xact_remove(ogs_sbi_xact_t *xact)
{
    ogs_sbi_object_t *sbi_object = NULL;

    ogs_assert(xact);

    sbi_object = xact->sbi_object;
    ogs_assert(sbi_object);

    if (xact->discovery_option)
        ogs_sbi_discovery_option_free(xact->discovery_option);

    ogs_assert(xact->t_response);
    ogs_timer_delete(xact->t_response);

    if (xact->request)
        ogs_sbi_request_free(xact->request);

    if (xact->target_apiroot)
        ogs_free(xact->target_apiroot);

    ogs_list_remove(&sbi_object->xact_list, xact);
    ogs_pool_free(&xact_pool, xact);
}

void ogs_sbi_xact_remove_all(ogs_sbi_object_t *sbi_object)
{
    ogs_sbi_xact_t *xact = NULL, *next_xact = NULL;

    ogs_assert(sbi_object);

    ogs_list_for_each_safe(&sbi_object->xact_list, next_xact, xact)
        ogs_sbi_xact_remove(xact);
}

ogs_sbi_xact_t *ogs_sbi_xact_cycle(ogs_sbi_xact_t *xact)
{
    return ogs_pool_cycle(&xact_pool, xact);
}

ogs_sbi_subscription_spec_t *ogs_sbi_subscription_spec_add(
        OpenAPI_nf_type_e nf_type, const char *service_name)
{
    ogs_sbi_subscription_spec_t *subscription_spec = NULL;

    ogs_assert(nf_type);

    ogs_pool_alloc(&subscription_spec_pool, &subscription_spec);
    ogs_assert(subscription_spec);
    memset(subscription_spec, 0, sizeof(ogs_sbi_subscription_spec_t));

    subscription_spec->subscr_cond.nf_type = nf_type;
    if (service_name)
        subscription_spec->subscr_cond.service_name = ogs_strdup(service_name);

    ogs_list_add(&ogs_sbi_self()->subscription_spec_list, subscription_spec);

    return subscription_spec;
}

void ogs_sbi_subscription_spec_remove(
        ogs_sbi_subscription_spec_t *subscription_spec)
{
    ogs_assert(subscription_spec);

    ogs_list_remove(&ogs_sbi_self()->subscription_spec_list, subscription_spec);

    if (subscription_spec->subscr_cond.service_name)
        ogs_free(subscription_spec->subscr_cond.service_name);

    ogs_pool_free(&subscription_spec_pool, subscription_spec);
}

void ogs_sbi_subscription_spec_remove_all(void)
{
    ogs_sbi_subscription_spec_t *subscription_spec = NULL;
    ogs_sbi_subscription_spec_t *next_subscription_spec = NULL;

    ogs_list_for_each_safe(&ogs_sbi_self()->subscription_spec_list,
            next_subscription_spec, subscription_spec)
        ogs_sbi_subscription_spec_remove(subscription_spec);
}

ogs_sbi_subscription_data_t *ogs_sbi_subscription_data_add(void)
{
    ogs_sbi_subscription_data_t *subscription_data = NULL;

    ogs_pool_alloc(&subscription_data_pool, &subscription_data);
    ogs_assert(subscription_data);
    memset(subscription_data, 0, sizeof(ogs_sbi_subscription_data_t));

    ogs_list_add(&ogs_sbi_self()->subscription_data_list, subscription_data);

    return subscription_data;
}

void ogs_sbi_subscription_data_set_id(
        ogs_sbi_subscription_data_t *subscription_data, char *id)
{
    ogs_assert(subscription_data);
    ogs_assert(id);

    subscription_data->id = ogs_strdup(id);
    ogs_assert(subscription_data->id);
}

void ogs_sbi_subscription_data_remove(
        ogs_sbi_subscription_data_t *subscription_data)
{
    ogs_assert(subscription_data);

    ogs_list_remove(&ogs_sbi_self()->subscription_data_list, subscription_data);

    if (subscription_data->id)
        ogs_free(subscription_data->id);

    if (subscription_data->notification_uri)
        ogs_free(subscription_data->notification_uri);

    if (subscription_data->req_nf_instance_id)
        ogs_free(subscription_data->req_nf_instance_id);

    if (subscription_data->subscr_cond.service_name)
        ogs_free(subscription_data->subscr_cond.service_name);

    if (subscription_data->t_validity)
        ogs_timer_delete(subscription_data->t_validity);

    if (subscription_data->t_patch)
        ogs_timer_delete(subscription_data->t_patch);

    if (subscription_data->client)
        ogs_sbi_client_remove(subscription_data->client);

    ogs_pool_free(&subscription_data_pool, subscription_data);
}

void ogs_sbi_subscription_data_remove_all_by_nf_instance_id(
        char *nf_instance_id)
{
    ogs_sbi_subscription_data_t *subscription_data = NULL;
    ogs_sbi_subscription_data_t *next_subscription_data = NULL;

    ogs_assert(nf_instance_id);

    ogs_list_for_each_safe(&ogs_sbi_self()->subscription_data_list,
            next_subscription_data, subscription_data) {
        if (subscription_data->req_nf_instance_id &&
            strcmp(subscription_data->req_nf_instance_id,
                nf_instance_id) == 0) {
            ogs_sbi_subscription_data_remove(subscription_data);
        }
    }
}

void ogs_sbi_subscription_data_remove_all(void)
{
    ogs_sbi_subscription_data_t *subscription_data = NULL;
    ogs_sbi_subscription_data_t *next_subscription_data = NULL;

    ogs_list_for_each_safe(&ogs_sbi_self()->subscription_data_list,
            next_subscription_data, subscription_data)
        ogs_sbi_subscription_data_remove(subscription_data);
}

ogs_sbi_subscription_data_t *ogs_sbi_subscription_data_find(char *id)
{
    ogs_sbi_subscription_data_t *subscription_data = NULL;

    ogs_assert(id);

    ogs_list_for_each(&ogs_sbi_self()->subscription_data_list,
            subscription_data) {
        ogs_assert(subscription_data->id);
        if (strcmp(subscription_data->id, id) == 0)
            break;
    }

    return subscription_data;
}

bool ogs_sbi_supi_in_vplmn(char *supi)
{
    char imsi_bcd[OGS_MAX_IMSI_BCD_LEN+1];
    bool home_network = false;
    int i;

    ogs_assert(supi);

    if (ogs_app()->num_of_serving_plmn_id == 0) {
        return false;
    }

    ogs_extract_digit_from_string(imsi_bcd, supi);

    for (i = 0; i < ogs_app()->num_of_serving_plmn_id; i++) {
        char buf[OGS_PLMNIDSTRLEN];
        ogs_plmn_id_to_string(&ogs_app()->serving_plmn_id[i], buf);

        if (strncmp(imsi_bcd, buf, strlen(buf)) == 0) {
            home_network = true;
            break;
        }
    }

    if (home_network == false)
        return true;

    return false;
}

bool ogs_sbi_plmn_id_in_vplmn(ogs_plmn_id_t *plmn_id)
{
    bool home_network = false;
    int i;

    ogs_assert(plmn_id);

    if (ogs_app()->num_of_serving_plmn_id == 0) {
        return false;
    }

    if (ogs_plmn_id_mcc(plmn_id) == 0) {
        ogs_error("No MCC");
        return false;
    }

    if (ogs_plmn_id_mnc(plmn_id) == 0) {
        ogs_error("No MNC");
        return false;
    }

    for (i = 0; i < ogs_app()->num_of_serving_plmn_id; i++) {
        if (memcmp(&ogs_app()->serving_plmn_id[i],
                    plmn_id, OGS_PLMN_ID_LEN) == 0) {
            home_network = true;
            break;
        }
    }

    if (home_network == false)
        return true;

    return false;
}

bool ogs_sbi_fqdn_in_vplmn(char *fqdn)
{
    bool home_network = false;
    int i;

    ogs_assert(fqdn);

    if (ogs_app()->num_of_serving_plmn_id == 0) {
        return false;
    }

    if (ogs_home_network_domain_from_fqdn(fqdn) == NULL) {
        return false;
    }

    for (i = 0; i < ogs_app()->num_of_serving_plmn_id; i++) {
        if (ogs_plmn_id_mcc_from_fqdn(fqdn) ==
            ogs_plmn_id_mcc(&ogs_app()->serving_plmn_id[i]) &&
            ogs_plmn_id_mnc_from_fqdn(fqdn) ==
            ogs_plmn_id_mnc(&ogs_app()->serving_plmn_id[i])) {
            home_network = true;
            break;
        }
    }

    if (home_network == false)
        return true;

    return false;
}
