/*
 * Copyright (C) 2023 by Sukchan Lee <acetcom@gmail.com>
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

#include "sbi-path.h"

#include "n32c-handler.h"

bool sepp_n32c_handshake_handle_security_capability_request(
    sepp_node_t *sepp_node,
    ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    OpenAPI_sec_negotiate_req_data_t *SecNegotiateReqData = NULL;

    OpenAPI_lnode_t *node = NULL;
    bool tls = false, prins = false, none = false;

    ogs_assert(sepp_node);
    ogs_assert(stream);
    ogs_assert(recvmsg);

    SecNegotiateReqData = recvmsg->SecNegotiateReqData;
    if (!SecNegotiateReqData) {
        ogs_error("[%s] No SecNegotiateReqData", sepp_node->fqdn);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "No SecNegotiateReqData", sepp_node->fqdn));
        return false;
    }

    if (!SecNegotiateReqData->sender) {
        ogs_error("[%s] No SecNegotiateReqData.sender", sepp_node->fqdn);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "No SecNegotiateReqData.sender", sepp_node->fqdn));
        return false;
    }

    ogs_assert(sepp_node->fqdn);
    if (strcmp(SecNegotiateReqData->sender, sepp_node->fqdn) != 0) {
        ogs_error("[%s] FQDN mismatch Sender [%s]",
                sepp_node->fqdn, SecNegotiateReqData->sender);
        return false;
    }

    if (!SecNegotiateReqData->supported_sec_capability_list) {
        ogs_error("[%s] No supported_sec_capability_list", sepp_node->fqdn);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "No supported_sec_capability_list", sepp_node->fqdn));
        return false;
    }

    OpenAPI_list_for_each(
            SecNegotiateReqData->supported_sec_capability_list, node) {
        OpenAPI_security_capability_e security_capability =
            (uintptr_t)node->data;
        if (security_capability == OpenAPI_security_capability_TLS)
            tls = true;
        else if (security_capability == OpenAPI_security_capability_PRINS)
            prins = true;
        else if (security_capability == OpenAPI_security_capability_NONE)
            none = true;
    }

    if (none == true) {
        sepp_node->negotiated_security_scheme =
            OpenAPI_security_capability_NONE;
    } else if (tls == true && sepp_self()->security_capability.tls == true) {
        sepp_node->negotiated_security_scheme =
            OpenAPI_security_capability_TLS;
    } else if (prins == true &&
            sepp_self()->security_capability.prins == true) {
        sepp_node->negotiated_security_scheme =
            OpenAPI_security_capability_PRINS;
    } else {
        OpenAPI_list_for_each(
                SecNegotiateReqData->supported_sec_capability_list, node) {
            OpenAPI_security_capability_e security_capability =
                (uintptr_t)node->data;
            ogs_error("[%s] Unknown SupportedSecCapability [%d]",
                    sepp_node->fqdn, security_capability);
        }
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                recvmsg, "Unknown SupportedSecCapability",
                sepp_node->fqdn));
        return false;
    }

    if (SecNegotiateReqData->is__3_gpp_sbi_target_api_root_supported == true &&
        SecNegotiateReqData->_3_gpp_sbi_target_api_root_supported == 1)
        sepp_node->target_apiroot_supported = true;

    sepp_node->num_of_plmn_id = 0;
    OpenAPI_list_for_each(SecNegotiateReqData->plmn_id_list, node) {
        OpenAPI_plmn_id_t *PlmnId = node->data;
        if (PlmnId) {
            ogs_sbi_parse_plmn_id(
                &sepp_node->plmn_id[sepp_node->num_of_plmn_id], PlmnId);
            sepp_node->num_of_plmn_id++;
        }
    }

    if (SecNegotiateReqData->target_plmn_id) {
        ogs_sbi_parse_plmn_id(
            &sepp_node->target_plmn_id, SecNegotiateReqData->target_plmn_id);
        sepp_node->target_plmn_id_presence = true;
    }

    if (SecNegotiateReqData->supported_features) {
        uint64_t supported_features =
            ogs_uint64_from_string(SecNegotiateReqData->supported_features);
        sepp_node->supported_features &= supported_features;
    } else {
        sepp_node->supported_features = 0;
    }

    return true;
}

bool sepp_n32c_handshake_handle_security_capability_response(
    sepp_node_t *sepp_node, ogs_sbi_message_t *recvmsg)
{
    OpenAPI_sec_negotiate_rsp_data_t *SecNegotiateRspData = NULL;

    OpenAPI_lnode_t *node = NULL;

    ogs_assert(sepp_node);
    ogs_assert(recvmsg);

    SecNegotiateRspData = recvmsg->SecNegotiateRspData;
    if (!SecNegotiateRspData) {
        ogs_error("[%s] No SecNegotiateRspData", sepp_node->fqdn);
        return false;
    }

    if (!SecNegotiateRspData->sender) {
        ogs_error("[%s] No SecNegotiateRspData.sender", sepp_node->fqdn);
        return false;
    }

    ogs_assert(sepp_node->fqdn);
    if (strcmp(SecNegotiateRspData->sender, sepp_node->fqdn) != 0) {
        ogs_error("[%s] FQDN mismatch Sender [%s]",
                sepp_node->fqdn, SecNegotiateRspData->sender);
        return false;
    }

    if (!SecNegotiateRspData->selected_sec_capability) {
        ogs_error("[%s] No selected_sec_capability", sepp_node->fqdn);
        return false;
    }

    sepp_node->negotiated_security_scheme =
        SecNegotiateRspData->selected_sec_capability;

    if (SecNegotiateRspData->is__3_gpp_sbi_target_api_root_supported == true &&
        SecNegotiateRspData->_3_gpp_sbi_target_api_root_supported == 1)
        sepp_node->target_apiroot_supported = true;

    sepp_node->num_of_plmn_id = 0;
    OpenAPI_list_for_each(SecNegotiateRspData->plmn_id_list, node) {
        OpenAPI_plmn_id_t *PlmnId = node->data;
        if (PlmnId) {
            ogs_sbi_parse_plmn_id(
                &sepp_node->plmn_id[sepp_node->num_of_plmn_id], PlmnId);
            sepp_node->num_of_plmn_id++;
        }
    }

    if (SecNegotiateRspData->supported_features) {
        uint64_t supported_features =
            ogs_uint64_from_string(SecNegotiateRspData->supported_features);
        sepp_node->supported_features &= supported_features;
    } else {
        sepp_node->supported_features = 0;
    }

    return true;
}
