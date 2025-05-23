/**
 * Copyright (c) 2021 Microsoft Open Technologies, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License"); you may
 *    not use this file except in compliance with the License. You may obtain
 *    a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 *    THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 *    CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 *    LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
 *    FOR A PARTICULAR PURPOSE, MERCHANTABILITY OR NON-INFRINGEMENT.
 *
 *    See the Apache Version 2.0 License for specific language governing
 *    permissions and limitations under the License.
 *
 *    Microsoft would like to thank the following companies for their review and
 *    assistance with these files: Intel Corporation, Mellanox Technologies Ltd,
 *    Dell Products, L.P., Facebook, Inc., Marvell International Ltd.
 *
 * @file    sai_rpc_frontend.cpp
 *
 * @brief   This module contains RPC server handler and helper functions
 */

#include <arpa/inet.h>

#include "sai_rpc.h"

extern "C" {
#include "saimetadata.h"
}

#include <iostream>
#include <cstring>

using namespace ::sai;

/**
 * @brief Convert Thrift MAC format to SAI MAC format
 */
static unsigned int sai_thrift_mac_t_parse(const std::string s, void *data)
{
    unsigned int i, j = 0;
    unsigned char *m = static_cast<unsigned char *>(data);
    memset(m, 0, 6);
    for (i = 0; i < s.size(); i++)
    {
        char let = s.c_str()[i];

        if (let >= '0' && let <= '9')
        {
            m[j / 2] = (unsigned char)((m[j / 2] << 4) + (let - '0'));
            j++;
        }
        else if (let >= 'a' && let <= 'f')
        {
            m[j / 2] = (unsigned char)((m[j / 2] << 4) + (let - 'a' + 10));
            j++;
        }
        else if (let >= 'A' && let <= 'F')
        {
            m[j / 2] = (unsigned char)((m[j / 2] << 4) + (let - 'A' + 10));
            j++;
        }
    }

    return (j == 12);
}

/**
 * @brief Convert Thrift IPv4 format to SAI IPv4 format
 */
static void sai_thrift_ip4_t_parse(const std::string s, unsigned int *m)
{
    unsigned char r = 0;
    unsigned int i;
    *m = 0;

    for (i = 0; i < s.size(); i++)
    {
        char let = s.c_str()[i];

        if (let >= '0' && let <= '9')
        {
            r = (unsigned char)((r * 10) + (let - '0'));
        }
        else
        {
            *m = (*m << 8) | r;
            r = 0;
        }
    }

    *m = (*m << 8) | (r & 0xFF);
    *m = htonl(*m);
}

/**
 * @brief Convert Thrift IPv6 format to SAI IPv6 format
 */
static void sai_thrift_ip6_t_parse(const std::string s, unsigned char *v6_ip)
{
    const char *v6_str = s.c_str();

    inet_pton(AF_INET6, v6_str, v6_ip);
}

/**
 * @brief Convert Thrift IP address format to SAI IP address format
 */
static void sai_thrift_ip_address_t_parse(
        const sai_thrift_ip_address_t &thrift_ip_address,
        sai_ip_address_t *ip_address)
{
    ip_address->addr_family = (sai_ip_addr_family_t)thrift_ip_address.addr_family;

    if ((sai_ip_addr_family_t)thrift_ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        sai_thrift_ip4_t_parse(thrift_ip_address.addr.ip4, &ip_address->addr.ip4);
    }
    else
    {
        sai_thrift_ip6_t_parse(thrift_ip_address.addr.ip6, ip_address->addr.ip6);
    }
}

/**
 * @brief Convert IP address and mask from Thrift to SAI format
 */
static void sai_thrift_ip_prefix_t_parse(
        const sai_thrift_ip_prefix_t &thrift_ip_prefix,
        sai_ip_prefix_t *ip_prefix)
{
    ip_prefix->addr_family = (sai_ip_addr_family_t)thrift_ip_prefix.addr_family;

    if ((sai_ip_addr_family_t)thrift_ip_prefix.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        sai_thrift_ip4_t_parse(thrift_ip_prefix.addr.ip4, &ip_prefix->addr.ip4);
        sai_thrift_ip4_t_parse(thrift_ip_prefix.mask.ip4, &ip_prefix->mask.ip4);
    }
    else
    {
        sai_thrift_ip6_t_parse(thrift_ip_prefix.addr.ip6, ip_prefix->addr.ip6);
        sai_thrift_ip6_t_parse(thrift_ip_prefix.mask.ip6, ip_prefix->mask.ip6);
    }
}

/**
 * @brief Convert u32 range from Thrift to SAI format
 */
static void sai_thrift_u32_range_t_parse(
        const sai_thrift_u32_range_t &thrift_u32_range,
        sai_u32_range_t *u32_range)
{
    u32_range->min = thrift_u32_range.min;
    u32_range->max = thrift_u32_range.max;
}

/**
 * @brief Convert s32 range from Thrift to SAI format
 */
static void sai_thrift_s32_range_t_parse(
        const sai_thrift_s32_range_t &thrift_s32_range,
        sai_s32_range_t *s32_range)
{
    s32_range->min = thrift_s32_range.min;
    s32_range->max = thrift_s32_range.max;
}

/**
 * @brief Convert attribute from Thrift to SAI format according to the type
 */
void convert_attr_thrift_to_sai(
        const sai_object_type_t ot,
        const sai_thrift_attribute_t &thrift_attr,
        sai_attribute_t *attr)
{
    const auto md = sai_metadata_get_attr_metadata(ot, thrift_attr.id);

    attr->id = thrift_attr.id;

    sai_thrift_exception e;

    e.status = SAI_STATUS_NOT_SUPPORTED;

    if (md == NULL)
    {
        SAI_META_LOG_ERROR("attr metadata not found for object type %d and attribute %d", ot, attr->id);
        e.status = SAI_STATUS_INVALID_PARAMETER;
        throw e;
    }

    switch (md->attrvaluetype)
    {
        case SAI_ATTR_VALUE_TYPE_BOOL:
            attr->value.booldata = thrift_attr.value.booldata;
            break;
        case SAI_ATTR_VALUE_TYPE_CHARDATA:
            // 32 is chardata size in sai types
            std::memcpy(attr->value.chardata, thrift_attr.value.chardata.c_str(), 32);
            break;
        case SAI_ATTR_VALUE_TYPE_UINT8:
            attr->value.u8 = thrift_attr.value.u8;
            break;
        case SAI_ATTR_VALUE_TYPE_INT8:
            attr->value.s8 = thrift_attr.value.s8;
            break;
        case SAI_ATTR_VALUE_TYPE_UINT16:
            attr->value.u16 = thrift_attr.value.u16;
            break;
        case SAI_ATTR_VALUE_TYPE_INT16:
            attr->value.s16 = thrift_attr.value.s16;
            break;
        case SAI_ATTR_VALUE_TYPE_UINT32:
            attr->value.u32 = thrift_attr.value.u32;
            break;
        case SAI_ATTR_VALUE_TYPE_INT32:
            attr->value.s32 = thrift_attr.value.s32;
            break;
        case SAI_ATTR_VALUE_TYPE_UINT64:
            attr->value.u64 = thrift_attr.value.u64;
            break;
        case SAI_ATTR_VALUE_TYPE_INT64:
            attr->value.s64 = thrift_attr.value.s64;
            break;
        case SAI_ATTR_VALUE_TYPE_MAC:
            sai_thrift_mac_t_parse(thrift_attr.value.mac, &attr->value.mac);
            break;
        case SAI_ATTR_VALUE_TYPE_IPV4:
            sai_thrift_ip4_t_parse(thrift_attr.value.ip4, &attr->value.ip4);
            break;
        case SAI_ATTR_VALUE_TYPE_IPV6:
            sai_thrift_ip6_t_parse(thrift_attr.value.ip6, attr->value.ip6);
            break;
        case SAI_ATTR_VALUE_TYPE_IP_ADDRESS:
            sai_thrift_ip_address_t_parse(thrift_attr.value.ipaddr, &attr->value.ipaddr);
            break;
        case SAI_ATTR_VALUE_TYPE_IP_PREFIX:
            sai_thrift_ip_prefix_t_parse(thrift_attr.value.ipprefix, &attr->value.ipprefix);
            break;
        case SAI_ATTR_VALUE_TYPE_OBJECT_ID:
            attr->value.oid = thrift_attr.value.oid;
            break;
        case SAI_ATTR_VALUE_TYPE_OBJECT_LIST:
            {
                attr->value.objlist.list = (sai_object_id_t *)malloc(sizeof(sai_object_id_t) * thrift_attr.value.objlist.count);
                int i = 0;
                for (auto obj : thrift_attr.value.objlist.idlist)
                {
                    attr->value.objlist.list[i++] = obj;
                }
                attr->value.objlist.count = thrift_attr.value.objlist.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_UINT8_LIST:
            {
                attr->value.u8list.list = (uint8_t *)malloc(sizeof(uint8_t) * thrift_attr.value.u8list.count);
                int i = 0;
                for (auto u8 : thrift_attr.value.u8list.uint8list)
                {
                    attr->value.u8list.list[i++] = u8;
                }
                attr->value.u8list.count = thrift_attr.value.u8list.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_INT8_LIST:
            {
                attr->value.s8list.list = (int8_t *)malloc(sizeof(int8_t) * thrift_attr.value.s8list.count);
                int i = 0;
                for (auto s8 : thrift_attr.value.s8list.int8list)
                {
                    attr->value.s8list.list[i++] = s8;
                }
                attr->value.s8list.count = thrift_attr.value.s8list.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_UINT16_LIST:
            {
                attr->value.u16list.list = (uint16_t *)malloc(sizeof(uint16_t) * thrift_attr.value.u16list.count);
                int i = 0;
                for (auto u16 : thrift_attr.value.u16list.uint16list)
                {
                    attr->value.u16list.list[i++] = u16;
                }
                attr->value.u16list.count = thrift_attr.value.u16list.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_INT16_LIST:
            {
                attr->value.s16list.list = (int16_t *)malloc(sizeof(int16_t) * thrift_attr.value.s16list.count);
                int i = 0;
                for (auto s16 : thrift_attr.value.s16list.int16list)
                {
                    attr->value.s16list.list[i++] = s16;
                }
                attr->value.s16list.count = thrift_attr.value.s16list.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_UINT32_LIST:
            {
                attr->value.u32list.list = (uint32_t *)malloc(sizeof(uint32_t) * thrift_attr.value.u32list.count);
                int i = 0;
                for (auto u32 : thrift_attr.value.u32list.uint32list)
                {
                    attr->value.u32list.list[i++] = u32;
                }
                attr->value.u32list.count = thrift_attr.value.u32list.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_INT32_LIST:
            {
                attr->value.s32list.list = (int32_t *)malloc(sizeof(int32_t) * thrift_attr.value.s32list.count);
                int i = 0;
                for (auto s32 : thrift_attr.value.s32list.int32list)
                {
                    attr->value.s32list.list[i++] = s32;
                }
                attr->value.s32list.count = thrift_attr.value.s32list.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_UINT32_RANGE:
            sai_thrift_u32_range_t_parse(thrift_attr.value.u32range, &attr->value.u32range);
            break;
        case SAI_ATTR_VALUE_TYPE_INT32_RANGE:
            sai_thrift_s32_range_t_parse(thrift_attr.value.s32range, &attr->value.s32range);
            break;
        case SAI_ATTR_VALUE_TYPE_UINT16_RANGE_LIST:
            {
                attr->value.u16rangelist.list = (sai_u16_range_t *)malloc(sizeof(sai_u16_range_t) * thrift_attr.value.u16rangelist.count);
                int i = 0;
                for (auto range : thrift_attr.value.u16rangelist.rangelist)
                {
                    attr->value.u16rangelist.list[i].min = range.min;
                    attr->value.u16rangelist.list[i++].max = range.max;
                }
                attr->value.u16rangelist.count = thrift_attr.value.u16rangelist.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_BOOL:
            attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
            attr->value.aclfield.data.booldata = thrift_attr.value.aclfield.data.booldata;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_UINT8:
            attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
            attr->value.aclfield.data.u8 = thrift_attr.value.aclfield.data.u8;
            attr->value.aclfield.mask.u8 = thrift_attr.value.aclfield.mask.u8;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_INT8:
            attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
            attr->value.aclfield.data.s8 = thrift_attr.value.aclfield.data.s8;
            attr->value.aclfield.mask.s8 = thrift_attr.value.aclfield.mask.s8;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_UINT16:
            attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
            attr->value.aclfield.data.u16 = thrift_attr.value.aclfield.data.u16;
            attr->value.aclfield.mask.u16 = thrift_attr.value.aclfield.mask.u16;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_INT16:
            attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
            attr->value.aclfield.data.s16 = thrift_attr.value.aclfield.data.s16;
            attr->value.aclfield.mask.s16 = thrift_attr.value.aclfield.mask.s16;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_UINT32:
            attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
            attr->value.aclfield.data.u32 = thrift_attr.value.aclfield.data.u32;
            attr->value.aclfield.mask.u32 = thrift_attr.value.aclfield.mask.u32;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_INT32:
            attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
            attr->value.aclfield.data.s32 = thrift_attr.value.aclfield.data.s32;
            attr->value.aclfield.mask.s32 = thrift_attr.value.aclfield.mask.s32;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_MAC:
            attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
            sai_thrift_mac_t_parse(thrift_attr.value.aclfield.data.mac, &attr->value.aclfield.data.mac);
            sai_thrift_mac_t_parse(thrift_attr.value.aclfield.mask.mac, &attr->value.aclfield.mask.mac);
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_IPV4:
            attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
            sai_thrift_ip4_t_parse(thrift_attr.value.aclfield.data.ip4, &attr->value.aclfield.data.ip4);
            sai_thrift_ip4_t_parse(thrift_attr.value.aclfield.mask.ip4, &attr->value.aclfield.mask.ip4);
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_IPV6:
            attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
            sai_thrift_ip6_t_parse(thrift_attr.value.aclfield.data.ip6, attr->value.aclfield.data.ip6);
            sai_thrift_ip6_t_parse(thrift_attr.value.aclfield.mask.ip6, attr->value.aclfield.mask.ip6);
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_OBJECT_ID:
            attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
            attr->value.aclfield.data.oid = thrift_attr.value.aclfield.data.oid;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_OBJECT_LIST:
            {
                int i = 0;
                attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
                attr->value.aclfield.data.objlist.list = (sai_object_id_t *)malloc(sizeof(sai_object_id_t) * thrift_attr.value.aclfield.data.objlist.count);
                for (auto obj : thrift_attr.value.aclfield.data.objlist.idlist)
                {
                    attr->value.aclfield.data.objlist.list[i++] = obj;
                }
                attr->value.aclfield.data.objlist.count = thrift_attr.value.aclfield.data.objlist.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_FIELD_DATA_UINT8_LIST:
            {
                int i = 0;
                attr->value.aclfield.enable = thrift_attr.value.aclfield.enable;
                attr->value.aclfield.data.u8list.list = (uint8_t *)malloc(sizeof(uint8_t) * thrift_attr.value.aclfield.data.u8list.count);
                for (auto obj : thrift_attr.value.aclfield.data.u8list.uint8list)
                {
                    attr->value.aclfield.data.u8list.list[i++] = obj;
                }
                attr->value.aclfield.data.u8list.count = thrift_attr.value.aclfield.data.u8list.count;
                i = 0;
                attr->value.aclfield.mask.u8list.list = (uint8_t *)malloc(sizeof(uint8_t) * thrift_attr.value.aclfield.mask.u8list.count);
                for (auto obj : thrift_attr.value.aclfield.mask.u8list.uint8list)
                {
                    attr->value.aclfield.mask.u8list.list[i++] = obj;
                }
                attr->value.aclfield.mask.u8list.count = thrift_attr.value.aclfield.mask.u8list.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_BOOL:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            attr->value.aclaction.parameter.booldata = thrift_attr.value.aclaction.parameter.booldata;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_UINT8:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            attr->value.aclaction.parameter.u8 = thrift_attr.value.aclaction.parameter.u8;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_INT8:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            attr->value.aclaction.parameter.s8 = thrift_attr.value.aclaction.parameter.s8;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_UINT16:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            attr->value.aclaction.parameter.u16 = thrift_attr.value.aclaction.parameter.u16;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_INT16:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            attr->value.aclaction.parameter.s16 = thrift_attr.value.aclaction.parameter.s16;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_UINT32:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            attr->value.aclaction.parameter.u32 = thrift_attr.value.aclaction.parameter.u32;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_INT32:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            attr->value.aclaction.parameter.s32 = thrift_attr.value.aclaction.parameter.s32;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_MAC:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            sai_thrift_mac_t_parse(thrift_attr.value.aclaction.parameter.mac, &attr->value.aclaction.parameter.mac);
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_IPV4:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            sai_thrift_ip4_t_parse(thrift_attr.value.aclaction.parameter.ip4, &attr->value.aclaction.parameter.ip4);
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_IPV6:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            sai_thrift_ip6_t_parse(thrift_attr.value.aclaction.parameter.ip6, attr->value.aclaction.parameter.ip6);
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_IP_ADDRESS:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            sai_thrift_ip_address_t_parse(thrift_attr.value.aclaction.parameter.ipaddr, &attr->value.aclaction.parameter.ipaddr);
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_OBJECT_ID:
            attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
            attr->value.aclaction.parameter.oid = thrift_attr.value.aclaction.parameter.oid;
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_ACTION_DATA_OBJECT_LIST:
            {
                int i = 0;
                attr->value.aclaction.enable = thrift_attr.value.aclaction.enable;
                attr->value.aclaction.parameter.objlist.list = (sai_object_id_t*)malloc(sizeof(sai_object_id_t) * thrift_attr.value.aclaction.parameter.objlist.count);
                for (auto obj : thrift_attr.value.aclaction.parameter.objlist.idlist)
                {
                    attr->value.aclaction.parameter.objlist.list[i++] = obj;
                }
                attr->value.aclaction.parameter.objlist.count = thrift_attr.value.aclaction.parameter.objlist.count;
            }
            break;

        case SAI_ATTR_VALUE_TYPE_ACL_CAPABILITY:
            {
                attr->value.aclcapability.is_action_list_mandatory = thrift_attr.value.aclcapability.is_action_list_mandatory;
                attr->value.aclcapability.action_list.list = (int32_t *)malloc(sizeof(int32_t) * thrift_attr.value.aclcapability.action_list.count);
                int i = 0;
                for (auto s32 : thrift_attr.value.aclcapability.action_list.int32list)
                {
                    attr->value.aclcapability.action_list.list[i++] = s32;
                }
                attr->value.aclcapability.action_list.count = thrift_attr.value.aclcapability.action_list.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_RESOURCE_LIST:
            {
                attr->value.aclresource.list = (sai_acl_resource_t *)malloc(sizeof(sai_acl_resource_t) * thrift_attr.value.aclresource.count);
                int i = 0;
                for (auto resource : thrift_attr.value.aclresource.resourcelist)
                {
                    attr->value.aclresource.list[i].stage = static_cast<sai_acl_stage_t>(resource.stage);
                    attr->value.aclresource.list[i].bind_point = static_cast<sai_acl_bind_point_type_t>(resource.bind_point);
                    attr->value.aclresource.list[i++].avail_num = resource.avail_num;
                }
                attr->value.aclresource.count = thrift_attr.value.aclresource.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_IP_ADDRESS_LIST:
            {
                attr->value.ipaddrlist.list = (sai_ip_address_t *)malloc(sizeof(sai_ip_address_t) * thrift_attr.value.ipaddrlist.count);
                int i = 0;
                for (auto address : thrift_attr.value.ipaddrlist.addresslist)
                {
                    sai_thrift_ip_address_t_parse(address, &attr->value.ipaddrlist.list[i++]);
                }
                attr->value.ipaddrlist.count = thrift_attr.value.ipaddrlist.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_IP_PREFIX_LIST:
            {
                attr->value.ipprefixlist.list = (sai_ip_prefix_t *)malloc(sizeof(sai_ip_prefix_t) * thrift_attr.value.ipprefixlist.count);
                int i = 0;
                for (auto address : thrift_attr.value.ipprefixlist.prefixlist)
                {
                    sai_thrift_ip_prefix_t_parse(address, &attr->value.ipprefixlist.list[i++]);
                }
                attr->value.ipprefixlist.count = thrift_attr.value.ipprefixlist.count;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_QOS_MAP_LIST:
            {
                attr->value.qosmap.list = (sai_qos_map_t *)malloc(sizeof(sai_qos_map_t) * thrift_attr.value.qosmap.count);
                int i = 0;
                for (auto qosmap : thrift_attr.value.qosmap.maplist)
                {
                    // key
                    attr->value.qosmap.list[i].key.tc = qosmap.key.tc;
                    attr->value.qosmap.list[i].key.dscp = qosmap.key.dscp;
                    attr->value.qosmap.list[i].key.dot1p = qosmap.key.dot1p;
                    attr->value.qosmap.list[i].key.prio = qosmap.key.prio;
                    attr->value.qosmap.list[i].key.pg = qosmap.key.pg;
                    attr->value.qosmap.list[i].key.queue_index = qosmap.key.queue_index;
                    attr->value.qosmap.list[i].key.color = static_cast<sai_packet_color_t>(qosmap.key.color);
                    attr->value.qosmap.list[i].key.mpls_exp = qosmap.key.mpls_exp;
                    // value
                    attr->value.qosmap.list[i].value.tc = qosmap.value.tc;
                    attr->value.qosmap.list[i].value.dscp = qosmap.value.dscp;
                    attr->value.qosmap.list[i].value.dot1p = qosmap.value.dot1p;
                    attr->value.qosmap.list[i].value.prio = qosmap.value.prio;
                    attr->value.qosmap.list[i].value.pg = qosmap.value.pg;
                    attr->value.qosmap.list[i].value.queue_index = qosmap.value.queue_index;
                    attr->value.qosmap.list[i].value.color = static_cast<sai_packet_color_t>(qosmap.value.color);
                    attr->value.qosmap.list[i].value.mpls_exp = qosmap.value.mpls_exp;
                    i++;
                }
                attr->value.qosmap.count = thrift_attr.value.qosmap.count;
            }
            break;
        default:
            SAI_META_LOG_ERROR("attr value type not supported for %s", md->attridname);
            throw e;
    }
}

/**
 * @brief Convert SAI IPv4 format to Thrift IPv4 format
 */
static std::string sai_ip4_t_to_thrift(const sai_ip4_t ip4)
{
    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(ip4), str, INET_ADDRSTRLEN);
    return str;
}

/**
 * @brief Convert SAI IPv6 format to Thrift IPv6 format
 */
static std::string sai_ip6_t_to_thrift(const sai_ip6_t ip6)
{
    char str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, ip6, str, INET6_ADDRSTRLEN);
    return str;
}

/**
 * @brief Convert SAI IP address format to Thrift IP address format
 */
static void sai_ip_address_t_to_thrift(sai_thrift_ip_address_t &thrift_ip, const sai_ip_address_t ip)
{
    if (ip.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        thrift_ip.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        thrift_ip.addr.ip4 = sai_ip4_t_to_thrift(ip.addr.ip4);
    }
    else if (ip.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
    {
        thrift_ip.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        thrift_ip.addr.ip6 = sai_ip6_t_to_thrift(ip.addr.ip6);
    }
}

/**
 * @brief Convert IP address and mask from SAI to Thrift format
 */
static void sai_ip_prefix_t_to_thrift(sai_thrift_ip_prefix_t &thrift_ip, const sai_ip_prefix_t ip)
{
    if (ip.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        thrift_ip.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        thrift_ip.addr.ip4 = sai_ip4_t_to_thrift(ip.addr.ip4);
        thrift_ip.mask.ip4 = sai_ip4_t_to_thrift(ip.mask.ip4);
    }
    else if (ip.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
    {
        thrift_ip.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        thrift_ip.addr.ip6 = sai_ip6_t_to_thrift(ip.addr.ip6);
        thrift_ip.mask.ip6 = sai_ip6_t_to_thrift(ip.mask.ip6);
    }
}

/**
 * @brief Convert attribute from SAI to Thrift format according to the type
 */
void convert_attr_sai_to_thrift(
        const sai_object_type_t ot,
        const sai_attribute_t &attr,
        sai_thrift_attribute_t &thrift_attr)
{
    const auto md = sai_metadata_get_attr_metadata(ot, attr.id);

    thrift_attr.id = attr.id;

    sai_thrift_exception e;

    e.status = SAI_STATUS_NOT_SUPPORTED;

    if (md == NULL)
    {
        SAI_META_LOG_ERROR("attr metadata not found for object type %d and attribute %d", ot, attr.id);
        e.status = SAI_STATUS_INVALID_PARAMETER;
        throw e;
    }

    switch (md->attrvaluetype)
    {
        case SAI_ATTR_VALUE_TYPE_BOOL:
            thrift_attr.value.booldata = attr.value.booldata;
            break;
        case SAI_ATTR_VALUE_TYPE_CHARDATA:
            thrift_attr.value.chardata = attr.value.chardata;
            break;
        case SAI_ATTR_VALUE_TYPE_UINT8:
            thrift_attr.value.u8 = attr.value.u8;
            break;
        case SAI_ATTR_VALUE_TYPE_INT8:
            thrift_attr.value.s8 = attr.value.s8;
            break;
        case SAI_ATTR_VALUE_TYPE_UINT16:
            thrift_attr.value.u16 = attr.value.u16;
            break;
        case SAI_ATTR_VALUE_TYPE_INT16:
            thrift_attr.value.s16 = attr.value.s16;
            break;
        case SAI_ATTR_VALUE_TYPE_UINT32:
            thrift_attr.value.u32 = attr.value.u32;
            break;
        case SAI_ATTR_VALUE_TYPE_INT32:
            thrift_attr.value.s32 = attr.value.s32;
            break;
        case SAI_ATTR_VALUE_TYPE_UINT64:
            thrift_attr.value.u64 = attr.value.u64;
            break;
        case SAI_ATTR_VALUE_TYPE_INT64:
            thrift_attr.value.s64 = attr.value.s64;
            break;
        case SAI_ATTR_VALUE_TYPE_MAC:
            {
                char mac_str[18];
                snprintf(mac_str,
                        sizeof(mac_str),
                        "%02x:%02x:%02x:%02x:%02x:%02x",
                        attr.value.mac[0],
                        attr.value.mac[1],
                        attr.value.mac[2],
                        attr.value.mac[3],
                        attr.value.mac[4],
                        attr.value.mac[5]);
                thrift_attr.value.mac = mac_str;
            }
            break;
        case SAI_ATTR_VALUE_TYPE_IPV4:
            thrift_attr.value.ip4 = sai_ip4_t_to_thrift(attr.value.ip4);
            break;
        case SAI_ATTR_VALUE_TYPE_IPV6:
            thrift_attr.value.ip6 = sai_ip6_t_to_thrift(attr.value.ip6);
            break;
        case SAI_ATTR_VALUE_TYPE_IP_ADDRESS:
            sai_ip_address_t_to_thrift(thrift_attr.value.ipaddr, attr.value.ipaddr);
            break;
        case SAI_ATTR_VALUE_TYPE_IP_PREFIX:
            sai_ip_prefix_t_to_thrift(thrift_attr.value.ipprefix, attr.value.ipprefix);
            break;
        case SAI_ATTR_VALUE_TYPE_OBJECT_ID:
            thrift_attr.value.oid = attr.value.oid;
            break;
        case SAI_ATTR_VALUE_TYPE_OBJECT_LIST:
            {
                for (unsigned int i = 0; i < attr.value.objlist.count; i++)
                {
                    thrift_attr.value.objlist.idlist.push_back(attr.value.objlist.list[i]);
                }
                thrift_attr.value.objlist.count = attr.value.objlist.count;
                free(attr.value.objlist.list);
            }
            break;
        case SAI_ATTR_VALUE_TYPE_UINT8_LIST:
            {
                for (unsigned int i = 0; i < attr.value.u8list.count; i++)
                {
                    thrift_attr.value.u8list.uint8list.push_back(attr.value.u8list.list[i]);
                }
                thrift_attr.value.u8list.count = attr.value.u8list.count;
                free(attr.value.u8list.list);
            }
            break;
        case SAI_ATTR_VALUE_TYPE_INT8_LIST:
            {
                for (unsigned int i = 0; i < attr.value.s8list.count; i++)
                {
                    thrift_attr.value.s8list.int8list.push_back(attr.value.s8list.list[i]);
                }
                thrift_attr.value.s8list.count = attr.value.s8list.count;
                free(attr.value.s8list.list);
            }
            break;
        case SAI_ATTR_VALUE_TYPE_UINT16_LIST:
            {
                for (unsigned int i = 0; i < attr.value.u16list.count; i++)
                {
                    thrift_attr.value.u16list.uint16list.push_back(attr.value.u16list.list[i]);
                }
                thrift_attr.value.u16list.count = attr.value.u16list.count;
                free(attr.value.u16list.list);
            }
            break;
        case SAI_ATTR_VALUE_TYPE_INT16_LIST:
            {
                for (unsigned int i = 0; i < attr.value.s16list.count; i++)
                {
                    thrift_attr.value.s16list.int16list.push_back(attr.value.s16list.list[i]);
                }
                thrift_attr.value.s16list.count = attr.value.s16list.count;
                free(attr.value.s16list.list);
            }
            break;
        case SAI_ATTR_VALUE_TYPE_UINT32_LIST:
            {
                for (unsigned int i = 0; i < attr.value.u32list.count; i++)
                {
                    thrift_attr.value.u32list.uint32list.push_back(attr.value.u32list.list[i]);
                }
                thrift_attr.value.u32list.count = attr.value.u32list.count;
                free(attr.value.u32list.list);
            }
            break;
        case SAI_ATTR_VALUE_TYPE_INT32_LIST:
            {
                for (unsigned int i = 0; i < attr.value.s32list.count; i++)
                {
                    thrift_attr.value.s32list.int32list.push_back(attr.value.s32list.list[i]);
                }
                thrift_attr.value.s32list.count = attr.value.s32list.count;
                free(attr.value.s32list.list);
            }
            break;
        case SAI_ATTR_VALUE_TYPE_UINT32_RANGE:
            thrift_attr.value.u32range.min = attr.value.u32range.min;
            thrift_attr.value.u32range.max = attr.value.u32range.max;
            break;
        case SAI_ATTR_VALUE_TYPE_INT32_RANGE:
            thrift_attr.value.s32range.min = attr.value.s32range.min;
            thrift_attr.value.s32range.max = attr.value.s32range.max;
            break;
        case SAI_ATTR_VALUE_TYPE_UINT16_RANGE_LIST:
            {
                for (unsigned int i = 0; i < attr.value.u16rangelist.count; i++)
                {
                    sai_thrift_u16_range_t range;
                    range.min = attr.value.u16rangelist.list[i].min;
                    range.max = attr.value.u16rangelist.list[i].max;
                    thrift_attr.value.u16rangelist.rangelist.push_back(range);
                }
                thrift_attr.value.u16rangelist.count = attr.value.u16rangelist.count;
                free(attr.value.u16rangelist.list);
            }
            break;

        case SAI_ATTR_VALUE_TYPE_ACL_CAPABILITY:
            {
                for (unsigned int i = 0; i < attr.value.aclcapability.action_list.count; i++)
                {
                    thrift_attr.value.aclcapability.action_list.int32list.push_back(attr.value.aclcapability.action_list.list[i]);
                }
                thrift_attr.value.aclcapability.action_list.count = attr.value.aclcapability.action_list.count;
                free(attr.value.aclcapability.action_list.list);
            }
            break;
        case SAI_ATTR_VALUE_TYPE_ACL_RESOURCE_LIST:
            {
                for (unsigned int i = 0; i < attr.value.aclresource.count; i++)
                {
                    sai_thrift_acl_resource_t resource = {};
                    resource.stage = attr.value.aclresource.list[i].stage;
                    resource.bind_point = attr.value.aclresource.list[i].bind_point;
                    resource.avail_num = attr.value.aclresource.list[i].avail_num;
                    thrift_attr.value.aclresource.resourcelist.push_back(resource);
                }
                thrift_attr.value.aclresource.count = attr.value.aclresource.count;
                free(attr.value.aclresource.list);
            }
            break;
        case SAI_ATTR_VALUE_TYPE_IP_ADDRESS_LIST:
            {
                for (unsigned int i = 0; i < attr.value.ipaddrlist.count; i++)
                {
                    sai_thrift_ip_address_t thrift_ip;
                    sai_ip_address_t_to_thrift(thrift_ip, attr.value.ipaddrlist.list[i]);
                    thrift_attr.value.ipaddrlist.addresslist.push_back(thrift_ip);
                }
                thrift_attr.value.ipaddrlist.count = attr.value.ipaddrlist.count;
                free(attr.value.ipaddrlist.list);
            }
            break;
        case SAI_ATTR_VALUE_TYPE_IP_PREFIX_LIST:
            {
                for (unsigned int i = 0; i < attr.value.ipprefixlist.count; i++)
                {
                    sai_thrift_ip_prefix_t thrift_ip;
                    sai_ip_prefix_t_to_thrift(thrift_ip, attr.value.ipprefix);
                    thrift_attr.value.ipprefixlist.prefixlist.push_back(thrift_ip);
                }
                thrift_attr.value.ipprefixlist.count = attr.value.ipprefixlist.count;
                free(attr.value.ipprefixlist.list);
            }
            break;
        case SAI_ATTR_VALUE_TYPE_QOS_MAP_LIST:
            {
                for (unsigned int i = 0; i < attr.value.qosmap.count; i++)
                {
                    sai_thrift_qos_map_t thrift_qos_map;
                    // key
                    thrift_qos_map.key.tc = attr.value.qosmap.list[i].key.tc;
                    thrift_qos_map.key.dscp = attr.value.qosmap.list[i].key.dscp;
                    thrift_qos_map.key.dot1p = attr.value.qosmap.list[i].key.dot1p;
                    thrift_qos_map.key.prio = attr.value.qosmap.list[i].key.prio;
                    thrift_qos_map.key.pg = attr.value.qosmap.list[i].key.pg;
                    thrift_qos_map.key.queue_index = attr.value.qosmap.list[i].key.queue_index;
                    thrift_qos_map.key.color = static_cast<int32_t>(attr.value.qosmap.list[i].key.color);
                    thrift_qos_map.key.mpls_exp = attr.value.qosmap.list[i].key.mpls_exp;
                    // value
                    thrift_qos_map.value.tc = attr.value.qosmap.list[i].value.tc;
                    thrift_qos_map.value.dscp = attr.value.qosmap.list[i].value.dscp;
                    thrift_qos_map.value.dot1p = attr.value.qosmap.list[i].value.dot1p;
                    thrift_qos_map.value.prio = attr.value.qosmap.list[i].value.prio;
                    thrift_qos_map.value.pg = attr.value.qosmap.list[i].value.pg;
                    thrift_qos_map.value.queue_index = attr.value.qosmap.list[i].value.queue_index;
                    thrift_qos_map.value.color = static_cast<int32_t>(attr.value.qosmap.list[i].value.color);
                    thrift_qos_map.value.mpls_exp = attr.value.qosmap.list[i].value.mpls_exp;
                    thrift_attr.value.qosmap.maplist.push_back(thrift_qos_map);
                }
                thrift_attr.value.qosmap.count = attr.value.qosmap.count;
                free(attr.value.qosmap.list);
            }
            break;
        default:
            SAI_META_LOG_ERROR("attr value type not supported for %s", md->attridname);
            throw e;
    }
}

/**
 * @brief Convert Thrift NAT type to SAI NAT type
 */
static void sai_thrift_nat_type_t_parse(
        const sai_thrift_nat_type_t &thrift_nat_type,
        sai_nat_type_t *nat_type)
{
    *nat_type = (sai_nat_type_t)thrift_nat_type;
}

// including it here we never have to modify the generated file
#include "sai_rpc_server.cpp"

class sai_rpcHandlerFrontend:
    virtual public sai_rpcHandler
{
    /**
     * @brief Thrift wrapper for sai_object_type_get_availability() SAI function
     */
    int64_t sai_thrift_object_type_get_availability(
            const sai_thrift_object_type_t object_type,
            const sai_thrift_attr_id_t attr_id,
            const int32_t attr_type) override
    {
        sai_attribute_t attr;
        attr.id = attr_id;
        attr.value.s32 = attr_type;
        uint32_t attr_count = 1;
        uint64_t count = 0;

        sai_object_type_get_availability(switch_id, (sai_object_type_t)object_type, attr_count, &attr, &count);
        return count;
    }

    /**
     * @brief Thrift wrapper for sai_object_type_query() SAI function
     */
    sai_thrift_object_type_t sai_thrift_object_type_query(
            const sai_thrift_object_id_t object_id) override
    {
        return sai_object_type_query(object_id);
    }

    /**
     * @brief Thrift wrapper for sai_switch_id_query() SAI function
     */
    sai_thrift_object_id_t sai_thrift_switch_id_query(
            const sai_thrift_object_id_t object_id) override
    {
        return sai_switch_id_query(object_id);
    }

    /**
     * @brief Thrift wrapper for sai_api_uninitialize() SAI function
     */
    sai_thrift_status_t sai_thrift_api_uninitialize(void) override
    {
        return sai_api_uninitialize();
    }

    /**
     * @brief Thrift wrapper for sai_query_attribute_enum_values_capability()
     *        function
     */
    void sai_thrift_query_attribute_enum_values_capability(
            std::vector<int32_t> &thrift_enum_caps,
            const sai_thrift_object_type_t object_type,
            const sai_thrift_attr_id_t attr_id,
            const int32_t caps_count) override
    {
        if (!caps_count)
        {
            return;
        }

        std::vector<int32_t> caps_list(caps_count);

        sai_s32_list_t enum_values_capability;

        enum_values_capability.list = caps_list.data();
        enum_values_capability.count = caps_count;

        sai_status_t status = sai_query_attribute_enum_values_capability(
                (sai_object_id_t)switch_id,
                (sai_object_type_t)object_type,
                (sai_attr_id_t)attr_id,
                &enum_values_capability);

        if (status == SAI_STATUS_SUCCESS)
        {
            for (uint32_t i = 0; i < enum_values_capability.count; ++i)
            {
                thrift_enum_caps.push_back(enum_values_capability.list[i]);
            }
        }
    }
};

static pthread_mutex_t cookie_mutex;
static pthread_cond_t cookie_cv;
static void *cookie;

/**
 * @brief Create a Thrift RPC server thread
 */
static void *sai_thrift_rpc_server_thread(void *arg)
{
    int port = *(int *)arg;

    std::shared_ptr<sai_rpcHandlerFrontend> handler(new sai_rpcHandlerFrontend());
    std::shared_ptr<TProcessor> processor(new sai_rpcProcessor(handler));
    std::shared_ptr<TServerTransport> serverTransport(new TServerSocket(port));
    std::shared_ptr<TTransportFactory> transportFactory(new TBufferedTransportFactory());
    std::shared_ptr<TProtocolFactory> protocolFactory(new TBinaryProtocolFactory());

    TSimpleServer server(processor, serverTransport, transportFactory, protocolFactory);
    pthread_mutex_lock(&cookie_mutex);
    cookie = (void *)processor.get();
    pthread_cond_signal(&cookie_cv);
    pthread_mutex_unlock(&cookie_mutex);
    server.serve();
    return 0;
}

static pthread_t sai_thrift_rpc_thread;

extern "C" {

    /**
     * @brief Start Thrift RPC server
     */
    int start_p4_sai_thrift_rpc_server(char *port)
    {
        static int *param = (int *)malloc(sizeof(int));
        *param = atoi(port);
        std::cerr << "Starting SAI RPC server on port " << port << std::endl;

        cookie = NULL;
        int status = pthread_create(&sai_thrift_rpc_thread, NULL, sai_thrift_rpc_server_thread, param);

        if (status)
        {
            return status;
        }

        pthread_mutex_lock(&cookie_mutex);

        while (!cookie)
        {
            pthread_cond_wait(&cookie_cv, &cookie_mutex);
        }

        pthread_mutex_unlock(&cookie_mutex);
        pthread_mutex_destroy(&cookie_mutex);
        pthread_cond_destroy(&cookie_cv);
        return status;
    }

    /**
     * @brief Start Thrift RPC server Wrapper
     */
    int start_sai_thrift_rpc_server(int port)
    {
        static char port_str[10];
        snprintf(port_str, sizeof(port_str), "%d", port);
        return start_p4_sai_thrift_rpc_server(port_str);
    }

    /**
     * @brief Stop Thrift RPC server
     */
    int stop_p4_sai_thrift_rpc_server(void)
    {
        int status = pthread_cancel(sai_thrift_rpc_thread);

        if (status == 0)
        {
            int s = pthread_join(sai_thrift_rpc_thread, NULL);

            if (s)
            {
                return s;
            }
        }

        return status;
    }
}
