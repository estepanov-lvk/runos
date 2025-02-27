/*
 * Copyright 2019 Applied Research Center for Computer Networks
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include "PacketParser.hpp"

#include <cstring>
#include <algorithm>
#include <runos/core/assert.hpp>

#include <boost/endian/arithmetic.hpp>
#include <fluid/of13msg.hh>

using namespace boost::endian;

namespace runos {

typedef boost::error_info< struct tag_oxm_ns, unsigned >
    errinfo_oxm_ns;
typedef boost::error_info< struct tag_oxm_field, unsigned >
    errinfo_oxm_field;

using ofb = of::oxm::basic_match_fields;

struct ethernet_hdr {
    big_uint48_t dst;
    big_uint48_t src;
    big_uint16_t type;

    size_t header_length() const
    { return sizeof(*this); }
};
static_assert(sizeof(ethernet_hdr) == 14, "");

struct dot1q_hdr {
    big_uint48_t dst;
    big_uint48_t src;
    big_uint16_t tpid;
    union {
        big_uint16_t tci;
        struct {
            uint8_t pcp:3;
            bool dei:1;
            uint16_t vid_unordered:12;
        };
    };
    big_uint16_t type;
};
static_assert(sizeof(dot1q_hdr) == 18, "");

struct ipv4_hdr {
    uint8_t ihl:4;
    uint8_t version:4;
    uint8_t ecn:2;
    uint8_t dscp:6;
    big_uint16_t total_len;
    big_uint16_t identification;
    struct {
        uint16_t flags:3;
        uint16_t fragment_offset_unordered:13;
    };

    big_uint8_t ttl;
    big_uint8_t protocol;
    big_uint16_t checksum;
    big_uint32_t src;
    big_uint32_t dst;

    size_t header_length() const
    { return ihl * 4; }
};
static_assert(sizeof(ipv4_hdr) == 20, "");

struct tcp_hdr {
    big_uint16_t src;
    big_uint16_t dst;
    big_uint32_t seq_no;
    big_uint32_t ack_no;
    uint8_t data_offset:4;
    uint8_t :3; // reserved
    bool NS:1;
    bool CWR:1;
    bool ECE:1;
    bool URG:1;
    bool ACK:1;
    bool PSH:1;
    bool RST:1;
    bool SYN:1;
    bool FIN:1;
    big_uint16_t window_size;
    big_uint16_t checksum;
    big_uint16_t urgent_pointer;
};
static_assert(sizeof(tcp_hdr) == 20, "");

struct udp_hdr {
    big_uint16_t src;
    big_uint16_t dst;
    big_uint16_t length;
    big_uint16_t checksum;

    size_t header_length() const { return 8;}
};
static_assert(sizeof(udp_hdr) == 8, "");

struct arp_hdr {
    big_uint16_t htype;
    big_uint16_t ptype;
    big_uint8_t hlen;
    big_uint8_t plen;
    big_uint16_t oper;
    big_uint48_t sha;
    big_uint32_t spa;
    big_uint48_t tha;
    big_uint32_t tpa;
};
static_assert(sizeof(arp_hdr) == 28, "");

struct dhcp_hdr {
    big_uint8_t op;
    big_uint8_t htype;
    big_uint8_t hlen;
    big_uint8_t hops;
    big_uint32_t xid;
    big_uint16_t secs;
    big_uint16_t flags;
    big_uint32_t ciaddr;
    big_uint32_t yiaddr;
    big_uint32_t siaddr;
    big_uint32_t giaddr;
    big_uint48_t chaddr;
    uint8_t options[];
};
static_assert(sizeof(dhcp_hdr) >= 34, "");

// TODO: make it more safe
// add size-checking of passed oxm type against field size
void PacketParser::bind(binding_list new_bindings)
{
    for (const auto& binding : new_bindings) {
        auto id = static_cast<size_t>(binding.first);
        ASSERT(!bindings.at(id), "Trying to bind already binded field {}", id);
        bindings.at(id) = binding.second;
    }
}

void PacketParser::rebind(binding_list new_bindings)
{
    for (const auto& binding : new_bindings) {
        auto id = static_cast<size_t>(binding.first);
        ASSERT(bindings.at(id), "Trying to rebind unbinded field {}", id);
        bindings.at(id) = binding.second;
    }
}

void PacketParser::parse_l2(uint8_t* data, size_t data_len)
{
    if (sizeof(ethernet_hdr) <= data_len) {
        eth = reinterpret_cast<ethernet_hdr*>(data);
        uint16_t l3_type;
        uint8_t dot1q_tag_size = 0;

        if (eth->type == 0x8100) {
            dot1q = reinterpret_cast<dot1q_hdr*>(data);
            vlan_tagged = true;
            bind({
                { ofb::ETH_TYPE, &dot1q->type/*tpid*/ },
                { ofb::ETH_SRC, &dot1q->src },
                { ofb::ETH_DST, &dot1q->dst },
                { ofb::VLAN_VID, &dot1q->tci }
            });
            l3_type = dot1q->type/*tpid*/;
            dot1q_tag_size = 4;
        } else {
            vlan_tagged = false;
            bind({
                { ofb::ETH_TYPE, &eth->type },
                { ofb::ETH_SRC, &eth->src },
                { ofb::ETH_DST, &eth->dst },
                { ofb::VLAN_VID, 0 }
            });
            l3_type = eth->type;
        }

        parse_l3(l3_type,
                 static_cast<uint8_t*>(data) + eth->header_length() + dot1q_tag_size,
                 data_len - eth->header_length());
    }
}

void PacketParser::parse_l3(uint16_t eth_type, uint8_t* data, size_t data_len)
{
    switch (eth_type) {
    case 0x0800: // ipv4
        if (sizeof(ipv4_hdr) <= data_len) {
            ipv4 = reinterpret_cast<ipv4_hdr*>(data);
            bind({
                { ofb::IP_PROTO, &ipv4->protocol },
                { ofb::IPV4_SRC, &ipv4->src },
                { ofb::IPV4_DST, &ipv4->dst }
            });

            if (data_len > ipv4->header_length()) {
                parse_l4(ipv4->protocol,
                         data + ipv4->header_length(),
                         data_len - ipv4->header_length());
            }
        }
        break;
    case 0x0806: // arp
        if (sizeof(arp_hdr) <= data_len) {
            arp = reinterpret_cast<arp_hdr*>(data);
            if (arp->htype != 1 ||
                arp->ptype != 0x0800 ||
                arp->hlen != 6 ||
                arp->plen != 4)
                break;

            bind({
                { ofb::ARP_OP, &arp->oper },
                { ofb::ARP_SHA, &arp->sha },
                { ofb::ARP_THA, &arp->tha },
                { ofb::ARP_SPA, &arp->spa },
                { ofb::ARP_TPA, &arp->tpa }
            });
        }
        break;
    case 0x86dd: // ipv6
        break;
    }
}

void PacketParser::parse_l4(uint8_t protocol, uint8_t* data, size_t data_len)
{
    switch (protocol) {
    case 0x06: // tcp
        if (sizeof(tcp_hdr) <= data_len) {
            tcp = reinterpret_cast<tcp_hdr*>(data);
            bind({
                { ofb::TCP_SRC, &tcp->src },
                { ofb::TCP_DST, &tcp->dst }
            });
        }
        break;
    case 0x11: // udp
        if (sizeof(udp_hdr) <= data_len) {
            udp = reinterpret_cast<udp_hdr*>(data);
            bind({
                { ofb::UDP_SRC, &udp->src },
                { ofb::UDP_DST, &udp->dst }
            });

            if (data_len > udp->header_length()) {
                if ((udp->src == 68) && (udp->dst == 67)) {
                    parse_dhcp(data + udp->header_length(),
                               data_len - udp->header_length());
                }
            }
        }
        break;
    case 0x01: // icmp
        break;
    }
}


void PacketParser::parse_dhcp(uint8_t* data, size_t data_len) {
    if (sizeof(dhcp_hdr) <= data_len) {
        dhcp = reinterpret_cast<dhcp_hdr*>(data);
        bind({
                     { ofb::DHCP_OP, &dhcp->op },
                     { ofb::DHCP_XID, &dhcp->xid },
                     { ofb::DHCP_CIADDR, &dhcp->ciaddr },
                     { ofb::DHCP_YIADDR, &dhcp->yiaddr },
                     { ofb::DHCP_CHADDR, &dhcp->chaddr }
             });

        // options parsing
        uint8_t* tmp = dhcp->options;
        bool flag = false;
        for(size_t i = 0; i <= data_len - 34; ++i) {
            tmp = dhcp->options + i;
            if (flag) {
                if (*tmp == 0xFF) break;
                this->dhcp_options[*tmp] = dhcp_opt(*tmp, *(tmp + 1), tmp + 2);
                i += *(tmp + 1) + 1;
            }
            if ((!flag) && (*tmp == 0x63))
                if ((i + 1 <= data_len - 34) &&(*(tmp + 1) == 0x82))
                    if ((i + 2 <= data_len - 34) &&(*(tmp + 2) == 0x53))
                        if ((i + 3 <= data_len - 34) &&(*(tmp + 3) == 0x63)) {
                            flag = true;
                            i += 3;
                        }
        }
    }
}

dhcp_opt PacketParser::get_dhcp_option(uint8_t code) {
    auto T = this->dhcp_options.find(code);
    if (T == this->dhcp_options.end()) {
        return dhcp_opt();
    } else {
        return this->dhcp_options[code];
    }
}

PacketParser::PacketParser(fluid_msg::of13::PacketIn& pi)
    : data(static_cast<uint8_t*>(pi.data()))
    , data_len(pi.data_len())
    , in_port(pi.match().in_port()->value())
{
    bindings.fill(nullptr);
    bind({
        { ofb::IN_PORT, &in_port }
    });

    if (data) {
        parse_l2(data, data_len);
    }
}

uint8_t* PacketParser::access(oxm::type t) const
{
    ASSERT(t.ns() == unsigned(of::oxm::ns::OPENFLOW_BASIC), "Unsupported oxm namespace: {}", t.ns());
    ASSERT(t.id() < bindings.size() && bindings[t.id()], "Unsupported oxm field: {}", t.id());

    return (uint8_t*) bindings[t.id()];
}

oxm::field<> PacketParser::load(oxm::mask<> mask) const
{
    auto value_bits = bits<>(mask.type().nbits(), access(mask.type()));
    return oxm::value<>{ mask.type(), value_bits } & mask;
}

void PacketParser::modify(oxm::field<> patch)
{
    oxm::field<> updated = 
        PacketParser::load(oxm::mask<>(patch.type())) >> patch;
    updated.value_bits().to_buffer(access(patch.type()));
}

bool PacketParser::vlanTagged()
{
    return vlan_tagged;
}

size_t PacketParser::serialize_to(size_t buffer_size, void* buffer) const
{
    size_t copied = std::min(data_len, buffer_size);
    std::memmove(buffer, data, copied);
    return copied;
}

size_t PacketParser::total_bytes() const
{
    return data_len;
}

} // namespace runos
